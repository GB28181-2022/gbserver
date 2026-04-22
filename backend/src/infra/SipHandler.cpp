/**
 * @file SipHandler.cpp
 * @brief 国标 SIP 业务逻辑实现
 * @details 实现 GB/T 28181-2016 协议的设备注册、鉴权、心跳处理：
 *          - REGISTER 请求的 Digest 鉴权（附录 F）
 *          - 黑白名单策略（whitelist/blacklist/normal）
 *          - 独立配置优先的鉴权逻辑
 *          - 心跳超时检测与级联状态更新
 * @date 2025
 * @note 依赖 SipDigest 模块进行鉴权校验
 */
#include "infra/SipHandler.h"
#include "infra/SipDigest.h"
#include "infra/DbUtil.h"
#include "infra/AuthHelper.h"
#include "infra/SipServerPjsip.h"  // 用于获取预读缓存的系统配置
#include "infra/SipCatalog.h"
#include "Util/logger.h"
#include <string>
#include <vector>
#include <algorithm>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <atomic>
#include <iostream>
#include <ctime>

using namespace toolkit;

namespace gb {

namespace {

/**
 * @brief 去除字符串首尾空白字符
 * @param s 输入字符串
 * @return 去除空白后的字符串
 */
std::string trim(const std::string& s) {
  size_t start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return "";
  size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

/**
 * @brief 获取系统配置的鉴权密码
 * @return 系统默认鉴权密码，未配置返回空字符串
 * @details 从预读缓存读取（高性能），若未加载则从数据库读取
 * @note 使用 SipServerPjsip 中的 SystemConfigCache 避免频繁查询数据库
 */
std::string getSystemAuthPassword() {
  // 使用 SipServerPjsip 的预读缓存（高性能，内存访问）
  return gb::getSystemPassword();
}

/**
 * @struct PlatformConfig
 * @brief 设备平台配置结构体
 * @details 存储从 device_platforms 表查询的配置信息：
 *          - listType: 名单类型（normal/whitelist/blacklist）
 *          - strategyMode: 鉴权策略（inherit/custom）
 *          - customAuthPassword: 独立配置的鉴权密码
 */
struct PlatformConfig {
  std::string listType;         /**< 名单类型：normal/whitelist/blacklist */
  std::string strategyMode;     /**< 鉴权策略：inherit继承系统配置，custom独立配置 */
  std::string customAuthPassword;  /**< 独立配置的鉴权密码 */
  bool dbOnline = false;        /**< 注册前数据库中的 online（用于判断是否需拉 Catalog） */
};

/**
 * @brief 解析 device_platforms 查询结果
 * @param out psql 查询输出（|分隔的字段）
 * @return 解析后的平台配置结构体
 * @details 解析格式：list_type|strategy_mode|custom_auth_password|online
 */
PlatformConfig parsePlatformConfig(const std::string& out) {
  PlatformConfig cfg;
  if (out.empty()) return cfg;

  size_t nl = out.find('\n');
  std::string line = (nl == std::string::npos) ? out : out.substr(0, nl);
  line = trim(line);

  std::vector<std::string> parts;
  std::string cur;
  for (char c : line) {
    if (c == '|') { parts.push_back(cur); cur.clear(); } else cur += c;
  }
  parts.push_back(cur);

  if (parts.size() >= 1) cfg.listType = trim(parts[0]);
  if (parts.size() >= 2) cfg.strategyMode = trim(parts[1]);
  if (parts.size() >= 3) cfg.customAuthPassword = trim(parts[2]);
  if (parts.size() >= 4) {
    std::string o = trim(parts[3]);
    cfg.dbOnline = (o == "t" || o == "true" || o == "1");
  }

  return cfg;
}

/** REGISTER/心跳落库：写入 signal_src_* 与 last_heartbeat_at（仅当源地址有效） */
static std::string sqlAppendSignalAndLastHb(const std::string& signalIp, int signalPort) {
  std::string s;
  if (!signalIp.empty() && signalPort > 0) {
    s += ", signal_src_ip='" + escapeSqlString(signalIp) + "'";
    s += ", signal_src_port=" + std::to_string(signalPort);
  }
  s += ", last_heartbeat_at=CURRENT_TIMESTAMP";
  return s;
}

/** Keepalive 节流：距上次落库最短间隔（秒），非每包写库 */
static constexpr int kKeepaliveDbFlushIntervalSec = 45;

static bool shouldCatalogResyncForAllOfflineCameras(const std::string& platformGbId) {
  if (platformGbId.empty()) return false;

  std::string sql =
      "SELECT COUNT(*), COUNT(*) FILTER (WHERE online = true) "
      "FROM cameras "
      "WHERE platform_id = (SELECT id FROM device_platforms WHERE gb_id='" +
      escapeSqlString(platformGbId) + "')";
  std::string out = execPsql(sql.c_str());
  if (out.empty()) return false;

  size_t nl = out.find('\n');
  std::string line = trim((nl == std::string::npos) ? out : out.substr(0, nl));
  if (line.empty()) return false;

  std::vector<std::string> parts;
  std::string cur;
  for (char c : line) {
    if (c == '|') {
      parts.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  parts.push_back(cur);
  if (parts.size() < 2) return false;

  const int totalCameras = std::atoi(trim(parts[0]).c_str());
  const int onlineCameras = std::atoi(trim(parts[1]).c_str());
  const bool shouldResync = (totalCameras > 0 && onlineCameras == 0);
  if (shouldResync) {
    InfoL << "【REGISTER】All cameras are offline before register, force Catalog resync for "
          << platformGbId << " total=" << totalCameras;
  }
  return shouldResync;
}

/** 注册触发 Catalog 的按平台冷却（仅注册路径，HTTP 手动查目录不受此限） */
static std::mutex g_registerCatalogMutex;
static std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_lastRegisterCatalogTime;

static void scheduleCatalogQueryAfterRegister(const std::string& platformGbId, bool needCatalog) {
  if (!needCatalog || platformGbId.empty()) return;
  if (!gb::isCatalogOnRegisterEnabled()) {
    InfoL << "【REGISTER】catalog_on_register disabled, skip " << platformGbId;
    return;
  }
  const int cooldownSec = gb::getCatalogOnRegisterCooldownSec();
  if (cooldownSec > 0) {
    std::lock_guard<std::mutex> lock(g_registerCatalogMutex);
    const auto now = std::chrono::steady_clock::now();
    auto it = g_lastRegisterCatalogTime.find(platformGbId);
    if (it != g_lastRegisterCatalogTime.end()) {
      const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count();
      if (elapsed < cooldownSec) {
        InfoL << "【REGISTER】Catalog cooldown " << elapsed << "s < " << cooldownSec << "s, skip "
              << platformGbId;
        return;
      }
    }
    g_lastRegisterCatalogTime[platformGbId] = now;
  }
  const int sn = static_cast<int>(std::time(nullptr));
  if (gb::sendCatalogQueryAsync(platformGbId, sn)) {
    InfoL << "【REGISTER】Catalog query queued for " << platformGbId << " sn=" << sn;
  }
}

/**
 * @brief 确定实际使用的鉴权密码
 * @param cfg 平台配置
 * @return 实际使用的鉴权密码
 * @details 独立配置优先逻辑：
 *          - 如果 strategy_mode='custom' 且 custom_auth_password 有值，使用独立配置
 *          - 否则使用系统全局配置密码
 */
std::string resolveAuthPassword(const PlatformConfig& cfg) {
  if (cfg.strategyMode == "custom" && !cfg.customAuthPassword.empty()) {
    return cfg.customAuthPassword;
  }
  return getSystemAuthPassword();
}

}  // namespace

/**
 * @brief 从 From 头或 URI 字符串解析设备 ID
 * @param fromHeader From 头字段值或 URI 字符串
 * @return 解析出的设备 ID
 * @details 支持格式：<sip:DeviceID@host> 或 sip:DeviceID@host
 *          按 GB28181 规范提取 DeviceID（URI 的用户部分）
 */
std::string parseDeviceIdFromFrom(const std::string& fromHeader) {
  size_t i = fromHeader.find("sip:");
  if (i == std::string::npos) return "";
  i += 4;
  size_t at = fromHeader.find('@', i);
  if (at == std::string::npos) return "";
  std::string id = fromHeader.substr(i, at - i);
  while (!id.empty() && id.back() == '>') id.pop_back();
  return trim(id);
}

/**
 * @brief 从 XML body 中提取 DeviceID
 * @param body XML 消息体
 * @return 提取的 DeviceID
 * @details 解析 <DeviceID>...</DeviceID> 标签内容
 */
std::string parseDeviceIdFromBody(const std::string& body) {
  const std::string tag = "<DeviceID>";
  size_t start = body.find(tag);
  if (start == std::string::npos) return "";
  start += tag.size();
  size_t end = body.find("</DeviceID>", start);
  if (end == std::string::npos) return "";
  return trim(body.substr(start, end - start));
}

/**
 * @brief 判断消息体是否为心跳消息
 * @param body XML 消息体
 * @return true 表示是心跳消息
 */
bool isKeepaliveBody(const std::string& body) {
  return body.find("<CmdType>Keepalive</CmdType>") != std::string::npos;
}

/**
 * @brief 检查心跳消息是否被允许
 * @param deviceId 设备 ID
 * @return HTTP 状态码
 * @details 检查流程：
 *          1. 查询设备配置（名单类型、鉴权策略、密码、在线状态）
 *          2. 黑名单直接拒绝（403）
 *          3. 白名单直接通过（200）
 *          4. 普通名单：配置了鉴权密码且当前不在线时需重新鉴权（401）
 *          5. 其他情况通过（200）
 */
int checkKeepaliveAuth(const std::string& deviceId) {
  if (deviceId.empty()) return 403;

  // 查询 list_type, strategy_mode, custom_auth_password, online
  std::string sql = "SELECT list_type, strategy_mode, custom_auth_password, online FROM device_platforms WHERE gb_id='" +
      escapeSqlString(deviceId) + "'";
  std::string out = execPsql(sql.c_str());
  if (out.empty()) return 403;

  size_t nl = out.find('\n');
  std::string line = (nl == std::string::npos) ? out : out.substr(0, nl);
  line = trim(line);

  std::string listType, strategyMode, customAuthPassword, onlineStr;
  {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : line) {
      if (c == '|') { parts.push_back(cur); cur.clear(); } else cur += c;
    }
    parts.push_back(cur);
    if (parts.size() >= 1) listType = trim(parts[0]);
    if (parts.size() >= 2) strategyMode = trim(parts[1]);
    if (parts.size() >= 3) customAuthPassword = trim(parts[2]);
    if (parts.size() >= 4) onlineStr = trim(parts[3]);
  }

  // 黑名单直接拒绝
  if (listType == "blacklist") return 403;

  // 白名单不需要鉴权
  if (listType == "whitelist") return 200;

  // 普通名单：检查是否需要鉴权（配置了密码且未在线）
  PlatformConfig cfg{listType, strategyMode, customAuthPassword};
  std::string authPassword = resolveAuthPassword(cfg);

  bool online = (onlineStr == "t" || onlineStr == "true" || onlineStr == "1");
  if (!authPassword.empty() && !online) {
    // 需要鉴权但当前不在线：要求重新 REGISTER 鉴权
    return 401;
  }
  return 200;
}

/**
 * @brief 处理 REGISTER 注册请求
 * @param deviceId 设备 ID
 * @param expires 过期时间（秒），0 表示注销
 * @param authHeader Authorization 头
 * @param method SIP 方法（如 "REGISTER"）
 * @param requestUri 请求 URI
 * @param contactIp Contact 头中的 IP 地址
 * @param contactPort Contact 头中的端口
 * @return HTTP 状态码
 * @details 完整处理流程：
 *          1. 黑白名单检查
 *          2. 鉴权校验（支持独立配置优先）
 *          3. 新设备自动注册（保存 Contact 信息）
 *          4. 更新在线状态（保存 Contact 地址）
 *          5. 注销时级联更新摄像头状态
 *          6. 新平台或从离线到在线时，按 gb_local_config 配置异步下发 Catalog Query（刷新 cameras 等）
 * @note 保存设备 Contact 地址到 contact_ip 和 contact_port 字段
 */
int handleRegister(const std::string& deviceId,
                   int expires,
                   const std::string& authHeader,
                   const std::string& method,
                   const std::string& requestUri,
                   const std::string& contactIp,
                   int contactPort,
                   const std::string& signalSrcIp,
                   int signalSrcPort) {
  if (deviceId.empty()) return 403;
  bool isLogout = (expires == 0);

  // 查询设备信息（含注册前 online，用于判断是否需主动 Catalog）
  std::string sql = "SELECT list_type, strategy_mode, custom_auth_password, online FROM device_platforms WHERE gb_id='" +
      escapeSqlString(deviceId) + "'";
  std::string out = execPsql(sql.c_str());

  PlatformConfig cfg;
  bool isNewDevice = out.empty();

  if (!isNewDevice) {
    cfg = parsePlatformConfig(out);

    // 黑名单：直接拒绝
    if (cfg.listType == "blacklist") return 403;

    // 白名单：不需要鉴权，直接通过
    if (cfg.listType == "whitelist") {
      // 白名单直接更新状态并返回（保存 Contact 信息）
      if (isLogout) {
        std::string upd = "UPDATE device_platforms SET online=false, updated_at=CURRENT_TIMESTAMP WHERE gb_id='"
            + escapeSqlString(deviceId) + "'";
        execPsqlCommand(upd);
      } else {
        const bool needCatalog = !cfg.dbOnline || shouldCatalogResyncForAllOfflineCameras(deviceId);
        // 构建更新语句，包含 Contact 地址
        std::string upd = "UPDATE device_platforms SET online=true, updated_at=CURRENT_TIMESTAMP";
        if (!contactIp.empty()) {
          upd += ", contact_ip='" + escapeSqlString(contactIp) + "'";
        }
        if (contactPort > 0) {
          upd += ", contact_port=" + std::to_string(contactPort);
        }
        upd += sqlAppendSignalAndLastHb(signalSrcIp, signalSrcPort);
        upd += " WHERE gb_id='" + escapeSqlString(deviceId) + "'";
        execPsqlCommand(upd);
        scheduleCatalogQueryAfterRegister(deviceId, needCatalog);
      }
      return 200;
    }

    // 普通名单：确定鉴权密码（独立配置优先）
    std::string authPassword = resolveAuthPassword(cfg);

    // 配置了鉴权密码：必须走 Digest 鉴权
    if (!authPassword.empty()) {
      if (authHeader.empty())
        return 401;
      std::string username, realm, nonce, uri, response;
      std::string algorithm;
      if (!parseDigestAuth(authHeader, username, realm, nonce, uri, response, &algorithm, nullptr))
        return 401;
      /* 校验时必须用 Authorization 里的 uri，与客户端计算 response 时一致 */
      if (!verifyDigestResponse(username, realm, authPassword, method,
                                uri, nonce, response, algorithm))
        return 401;
    }
    // 未配置密码：不需要鉴权，直接通过
  } else {
    // 新设备：注销请求直接返回成功
    if (isLogout)
      return 200;

    // 检查系统是否配置了全局鉴权密码
    std::string systemPassword = getSystemAuthPassword();
    
    // 如果配置了系统密码，新设备也需要走鉴权流程
    if (!systemPassword.empty()) {
      // 系统配置了密码，新设备也需要鉴权
      if (authHeader.empty()) {
        // 没有提供鉴权信息，返回 401 要求鉴权
        return 401;
      }
      
      // 验证鉴权信息
      std::string username, realm, nonce, uri, response;
      std::string algorithm;
      if (!parseDigestAuth(authHeader, username, realm, nonce, uri, response, &algorithm, nullptr)) {
        return 401;
      }
      if (!verifyDigestResponse(username, realm, systemPassword, method,
                                uri, nonce, response, algorithm)) {
        return 401;
      }
      // 鉴权通过，继续创建设备
    }
    
    // 未配置系统密码：不需要鉴权，直接创建设备

    // 自动创建设备（inherit 模式），保存 Contact 信息
    std::string insertSql = "INSERT INTO device_platforms (name, gb_id, list_type, strategy_mode, online";
    std::string valuesSql = "VALUES ('" + escapeSqlString(deviceId) + "','" + escapeSqlString(deviceId) + "','normal','inherit',true";
    
    // 添加 Contact 地址字段
    if (!contactIp.empty()) {
      insertSql += ", contact_ip";
      valuesSql += ",'" + escapeSqlString(contactIp) + "'";
    }
    if (contactPort > 0) {
      insertSql += ", contact_port";
      valuesSql += "," + std::to_string(contactPort);
    }
    if (!signalSrcIp.empty() && signalSrcPort > 0) {
      insertSql += ", signal_src_ip, signal_src_port";
      valuesSql += ",'" + escapeSqlString(signalSrcIp) + "'," + std::to_string(signalSrcPort);
    }
    insertSql += ", last_heartbeat_at";
    valuesSql += ",CURRENT_TIMESTAMP";

    insertSql += ") " + valuesSql + ")";
    
    if (!execPsqlCommand(insertSql)) return 403;
    scheduleCatalogQueryAfterRegister(deviceId, true);
    return 200;
  }

  // 更新在线状态
  if (isLogout) {
    // 平台注销，先查询平台ID，然后更新平台状态，并级联更新其下摄像头为离线
    std::string getIdSql = "SELECT id FROM device_platforms WHERE gb_id='" + escapeSqlString(deviceId) + "'";
    std::string platformId = execPsql(getIdSql.c_str());
    trim(platformId);
    
    // 更新平台为离线
    std::string upd = "UPDATE device_platforms SET online=false, updated_at=CURRENT_TIMESTAMP WHERE gb_id='"
        + escapeSqlString(deviceId) + "'";
    execPsqlCommand(upd);
    
    // 级联更新该平台下所有摄像头为离线
    if (!platformId.empty()) {
      std::string updCameras = "UPDATE cameras SET online=false, updated_at=CURRENT_TIMESTAMP WHERE platform_id=" + platformId;
      execPsqlCommand(updCameras);
      std::cout << "Platform " << deviceId << " offline, cascaded " << platformId << " cameras to offline" << std::endl;
    }
  } else {
    const bool needCatalog = !cfg.dbOnline || shouldCatalogResyncForAllOfflineCameras(deviceId);
    // 平台注册，更新平台状态为在线，同时保存 Contact 地址
    std::string upd = "UPDATE device_platforms SET online=true, updated_at=CURRENT_TIMESTAMP";
    if (!contactIp.empty()) {
      upd += ", contact_ip='" + escapeSqlString(contactIp) + "'";
    }
    if (contactPort > 0) {
      upd += ", contact_port=" + std::to_string(contactPort);
    }
    upd += sqlAppendSignalAndLastHb(signalSrcIp, signalSrcPort);
    upd += " WHERE gb_id='" + escapeSqlString(deviceId) + "'";
    execPsqlCommand(upd);
    scheduleCatalogQueryAfterRegister(deviceId, needCatalog);
  }
  return 200;
}

/** Keepalive 落库节流：记录上次 flush 与已写入的 signal，映射变化时立即刷新 */
struct HeartbeatDbFlushTracker {
  std::chrono::steady_clock::time_point lastFlushDb{};
  bool hasFlushed = false;
  std::string lastWrittenSignalIp;
  int lastWrittenSignalPort = 0;
};

// 心跳内存缓存 - 高性能，避免每次心跳都写数据库
struct HeartbeatCache {
  std::unordered_map<std::string, std::chrono::steady_clock::time_point> lastHeartbeat;
  std::unordered_set<std::string> onlineDevices;
  std::unordered_map<std::string, HeartbeatDbFlushTracker> flushTrackers;
  std::mutex mutex;
  
  // 收到心跳时调用（signal 为 UDP 源地址，供 NAT 下行与节流落库）
  void onHeartbeat(const std::string& deviceId, const std::string& signalIp, int signalPort) {
    std::lock_guard<std::mutex> lock(mutex);
    auto now = std::chrono::steady_clock::now();
    
    bool wasOnline = (onlineDevices.find(deviceId) != onlineDevices.end());
    lastHeartbeat[deviceId] = now;
    onlineDevices.insert(deviceId);

    const bool haveSignal = !signalIp.empty() && signalPort > 0;
    HeartbeatDbFlushTracker& tr = flushTrackers[deviceId];
    const bool signalChanged =
        haveSignal &&
        (signalIp != tr.lastWrittenSignalIp || signalPort != tr.lastWrittenSignalPort);
    const auto minGap = std::chrono::seconds(kKeepaliveDbFlushIntervalSec);
    const bool intervalElapsed =
        tr.hasFlushed && (now - tr.lastFlushDb >= minGap);

    const bool needDb = !wasOnline || signalChanged || intervalElapsed;
    if (!needDb)
      return;

    std::string upd = "UPDATE device_platforms SET online=true, updated_at=CURRENT_TIMESTAMP";
    if (haveSignal) {
      upd += ", signal_src_ip='" + escapeSqlString(signalIp) + "'";
      upd += ", signal_src_port=" + std::to_string(signalPort);
      tr.lastWrittenSignalIp = signalIp;
      tr.lastWrittenSignalPort = signalPort;
    }
    upd += ", last_heartbeat_at=CURRENT_TIMESTAMP WHERE gb_id='" + escapeSqlString(deviceId) + "'";
    execPsqlCommand(upd);
    tr.hasFlushed = true;
    tr.lastFlushDb = now;
  }
  
  // 检查设备是否在线（用于超时检查）
  bool isOnline(const std::string& deviceId) {
    std::lock_guard<std::mutex> lock(mutex);
    return onlineDevices.find(deviceId) != onlineDevices.end();
  }
  
  // 获取设备最后心跳时间
  std::chrono::steady_clock::time_point getLastHeartbeat(const std::string& deviceId) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = lastHeartbeat.find(deviceId);
    if (it != lastHeartbeat.end()) {
      return it->second;
    }
    return std::chrono::steady_clock::time_point();  // 返回空时间点
  }
  
  // 将设备标记为离线
  void markOffline(const std::string& deviceId) {
    std::lock_guard<std::mutex> lock(mutex);
    onlineDevices.erase(deviceId);
    flushTrackers.erase(deviceId);
  }
  
  // 获取所有在线设备列表（用于超时检查）
  std::vector<std::string> getOnlineDevices() {
    std::lock_guard<std::mutex> lock(mutex);
    return std::vector<std::string>(onlineDevices.begin(), onlineDevices.end());
  }
};

// 全局心跳缓存实例
static HeartbeatCache g_heartbeatCache;

/**
 * @brief 处理心跳消息（高性能版本）
 * @param deviceId 设备 ID
 * @details 更新内存缓存中的心跳时间，只在状态变化时（离线->在线）写入数据库
 *          避免每次心跳都写数据库，提升性能
 */
void handleKeepalive(const std::string& deviceId,
                     const std::string& signalSrcIp,
                     int signalSrcPort) {
  if (deviceId.empty()) return;
  g_heartbeatCache.onHeartbeat(deviceId, signalSrcIp, signalSrcPort);
}

// 心跳超时检查变量（在命名空间内）
static std::thread g_timeoutThread;
static std::atomic<bool> g_timeoutRunning{false};
static int g_timeoutSeconds = 120;

/**
 * @brief 心跳超时检查线程函数（高性能版本）
 * @details 使用内存缓存检查超时设备，只在状态变化时写入数据库
 *          避免频繁查询数据库，提升性能
 */
static void heartbeatTimeoutChecker() {
  std::cout << "Heartbeat timeout checker started, timeout=" << g_timeoutSeconds << "s" << std::endl;
  
  while (g_timeoutRunning) {
    // 每隔 timeout/2 秒检查一次
    for (int i = 0; i < g_timeoutSeconds / 2 && g_timeoutRunning; i++) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    if (!g_timeoutRunning) break;
    
    // 使用内存缓存检查超时设备（高性能）
    auto now = std::chrono::steady_clock::now();
    auto timeoutThreshold = std::chrono::seconds(g_timeoutSeconds);
    
    // 获取所有内存中标记为在线的设备
    std::vector<std::string> onlineDevices = g_heartbeatCache.getOnlineDevices();
    
    for (const auto& gbId : onlineDevices) {
      auto lastHb = g_heartbeatCache.getLastHeartbeat(gbId);
      
      // 检查是否超时
      if (now - lastHb > timeoutThreshold) {
        // 设备超时，标记为离线
        g_heartbeatCache.markOffline(gbId);
        
        // 查询平台ID并更新数据库为离线
        std::string sql = "SELECT id FROM device_platforms WHERE gb_id='" + 
            gb::escapeSqlString(gbId) + "'";
        std::string idStr = gb::execPsql(sql.c_str());
        
        if (!idStr.empty()) {
          // 去除换行符
          size_t nl = idStr.find('\n');
          if (nl != std::string::npos) idStr = idStr.substr(0, nl);
          
          // 更新平台为离线
          std::string updPlatform = "UPDATE device_platforms SET online=false, updated_at=CURRENT_TIMESTAMP WHERE id=" + idStr;
          if (gb::execPsqlCommand(updPlatform)) {
            std::cout << "Platform " << gbId << " (id=" << idStr << ") timed out, marked offline" << std::endl;
            
            // 级联更新该平台下所有摄像头为离线
            std::string updCameras = "UPDATE cameras SET online=false, updated_at=CURRENT_TIMESTAMP WHERE platform_id=" + idStr;
            gb::execPsqlCommand(updCameras);
            std::cout << "Cascaded cameras of platform " << gbId << " to offline" << std::endl;
          }
        }
      }
    }
  }
  
  std::cout << "Heartbeat timeout checker stopped" << std::endl;
}

/**
 * @brief 启动心跳超时检查线程
 * @param timeoutSeconds 超时时间（秒），默认 120 秒
 * @details 创建后台线程定期检查设备超时，
 *          超时设备将被标记为离线，并级联更新其摄像头
 */
void startHeartbeatTimeoutChecker(int timeoutSeconds) {
  if (g_timeoutRunning) return;
  
  g_timeoutSeconds = timeoutSeconds > 0 ? timeoutSeconds : 120;
  g_timeoutRunning = true;
  g_timeoutThread = std::thread(heartbeatTimeoutChecker);
}

/**
 * @brief 停止心跳超时检查线程
 * @details 设置停止标志并等待线程结束
 */
void stopHeartbeatTimeoutChecker() {
  g_timeoutRunning = false;
  if (g_timeoutThread.joinable()) {
    g_timeoutThread.join();
  }
}

}  // namespace gb
