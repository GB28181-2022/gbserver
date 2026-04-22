/**
 * @file SipServerPjsip.cpp
 * @brief PJSIP SIP 服务器实现
 * @details 实现 GB/T 28181-2016 协议的 SIP 信令处理：
 *          - REGISTER 注册/注销处理
 *          - MESSAGE（Keepalive、Catalog）处理
 *          - NOTIFY（目录主动上报）处理
 *          - Digest 鉴权响应（401）发送
 *          - SIP 事件循环处理
 * @date 2025
 * @note 依赖 PJSIP 协议栈，与 SipHandler 协同处理业务逻辑
 */
#include "infra/SipServerPjsip.h"
#include "infra/SipHandler.h"
#include "infra/SipCatalog.h"
#include "infra/DbUtil.h"
#include "infra/UpstreamRegistrar.h"
#include "infra/UpstreamInviteBridge.h"
#include "infra/UpstreamPlatformService.h"
#include "Util/logger.h"
#include <pjsip.h>
#include <pjsip_simple.h>
#include <pjsip/sip_transaction.h>
#include <pjlib.h>
#include <pjlib-util.h>
#include <cstring>
#include <strings.h>  // for strncasecmp (POSIX)
#include <thread>
#include <atomic>
#include <chrono>
#include <sstream>
#include <cstdlib>
#include <unordered_map>
#include <mutex>

using namespace toolkit;

namespace gb {

/**
 * @brief 从入站报文取信令源地址（NAT 出口）
 * @note 优先 pkt_info.src_name/src_port；部分场景仅填充 src_addr，需回退打印
 */
void extractSignalSrcFromRdata(pjsip_rx_data* rdata, std::string& outIp, int& outPort) {
  outIp.clear();
  outPort = 0;
  if (!rdata) return;
  if (rdata->pkt_info.src_name[0] != '\0') {
    outIp.assign(rdata->pkt_info.src_name);
    outPort = rdata->pkt_info.src_port;
    return;
  }
  if (rdata->pkt_info.src_addr_len > 0) {
    char buf[PJ_INET6_ADDRSTRLEN + 8];
    char* p = pj_sockaddr_print(&rdata->pkt_info.src_addr, buf, (int)sizeof(buf), 0);
    if (p && p[0] != '\0') {
      outIp.assign(p);
      outPort = (int)pj_sockaddr_get_port(&rdata->pkt_info.src_addr);
    }
  }
}

namespace {


/**
 * @brief 为 INVITE 的 ACK 预置 UDP 目的地址（与 Catalog/INVITE 的 signal_src 策略一致）
 * @param rdata 收到的 200 OK（用于 pkt_info.src_addr 与 Via/Received 回退）
 * @param tdata 待发送的 ACK 事务数据，写入 dest_info.addr
 * @details 200 OK 经 NAT 到达时 Contact 常为内网地址，stateless 按 R-URI 发包会丢 ACK。
 *          优先使用本包 pkt_info.src_addr（对端真实信令出口）；否则回退 extractSignalSrcFromRdata。
 */
static void presetAckUdpDestFromInviteResponse(pjsip_rx_data* rdata, pjsip_tx_data* tdata) {
  tdata->dest_info.cur_addr = 0;
  pjsip_server_address_record* rec = &tdata->dest_info.addr.entry[0];
  pj_bzero(rec, sizeof(*rec));

  if (rdata && rdata->pkt_info.src_addr_len > 0) {
    pj_sockaddr_cp(&rec->addr, &rdata->pkt_info.src_addr);
    rec->addr_len = pj_sockaddr_get_len(&rec->addr);
    rec->type = PJSIP_TRANSPORT_UDP;
    rec->priority = 0;
    rec->weight = 0;
    tdata->dest_info.addr.count = 1;
    char abuf[PJ_INET6_ADDRSTRLEN + 16];
    pj_sockaddr_print(&rec->addr, abuf, (int)sizeof(abuf), 3);
    InfoL << "【sendAckForInvite】预置 UDP 目的为 200 OK 来源地址: " << abuf;
    return;
  }

  std::string ip;
  int port = 0;
  extractSignalSrcFromRdata(rdata, ip, port);
  if (!ip.empty() && port > 0) {
    pj_str_t sh = pj_str((char*)ip.c_str());
    pj_status_t pst;
#if defined(PJ_HAS_IPV6) && PJ_HAS_IPV6
    if (strchr(ip.c_str(), ':') != nullptr)
      pst = pj_sockaddr_init(pj_AF_INET6(), &rec->addr, &sh, (pj_uint16_t)port);
    else
#endif
      pst = pj_sockaddr_init(pj_AF_INET(), &rec->addr, &sh, (pj_uint16_t)port);
    if (pst == PJ_SUCCESS) {
      rec->addr_len = pj_sockaddr_get_len(&rec->addr);
      rec->type = PJSIP_TRANSPORT_UDP;
      rec->priority = 0;
      rec->weight = 0;
      tdata->dest_info.addr.count = 1;
      InfoL << "【sendAckForInvite】预置 UDP 目的(回退): " << ip << ":" << port;
      return;
    }
  }

  tdata->dest_info.addr.count = 0;
  WarnL << "【sendAckForInvite】无法预置 UDP 目的，将按 R-URI/Contact 解析（NAT 下 ACK 可能无法到达下级）";
}

}  // namespace

// SIP 端点（全局，供 Catalog 模块使用）
pjsip_endpoint* g_sip_endpt = nullptr;

namespace {

// 配置
SipServerConfig g_config;
std::atomic<bool> g_running{false};
std::thread g_workerThread;

// 全局缓存内存池，用于创建 PJSIP endpoint
static pj_caching_pool g_cp;
static bool g_cp_inited = false;

// PJSIP 模块 ID（后面完整定义）

// 系统级配置缓存（高性能，避免每次读取数据库）
struct SystemConfigCache {
    std::string realm;        // SIP域，用于401响应
    std::string localGbId;    // 本机国标ID
    std::string password;     // 鉴权密码
    int signalPort = 5060;    // SIP端口
    bool catalogOnRegisterEnabled = true;
    int catalogOnRegisterCooldownSec = 60;
    bool loaded = false;      // 是否已加载
    
    // 从数据库加载配置
    void loadFromDb() {
        // 读取 SIP 域（realm）
        std::string sql = "SELECT COALESCE(domain, '3402000000') FROM gb_local_config LIMIT 1";
        std::string out = execPsql(sql.c_str());
        realm = trimString(out);
        if (realm.empty()) realm = "3402000000";
        
        // 读取本机国标ID
        sql = "SELECT COALESCE(gb_id, '34020000002000000001') FROM gb_local_config LIMIT 1";
        out = execPsql(sql.c_str());
        localGbId = trimString(out);
        if (localGbId.empty()) localGbId = "34020000002000000001";
        
        // 读取密码
        sql = "SELECT COALESCE(password, 'admin') FROM gb_local_config LIMIT 1";
        out = execPsql(sql.c_str());
        password = trimString(out);
        if (password.empty()) password = "admin";

        // 注册成功后自动 Catalog（列缺失时由迁移脚本补齐；查询失败则保持默认）
        sql = "SELECT COALESCE(catalog_on_register_enabled, true) FROM gb_local_config LIMIT 1";
        out = execPsql(sql.c_str());
        if (!out.empty()) {
            std::string v = trimString(out);
            catalogOnRegisterEnabled = (v == "t" || v == "true" || v == "1");
        } else {
            catalogOnRegisterEnabled = true;
        }
        sql = "SELECT COALESCE(catalog_on_register_cooldown_sec, 60) FROM gb_local_config LIMIT 1";
        out = execPsql(sql.c_str());
        if (!out.empty()) {
            int c = std::atoi(trimString(out).c_str());
            catalogOnRegisterCooldownSec = c >= 0 ? c : 60;
        } else {
            catalogOnRegisterCooldownSec = 60;
        }
        
        loaded = true;
        InfoL << "【SystemConfigCache】Loaded from DB: realm=" << realm 
              << ", gbId=" << localGbId << ", password=" << (password.empty() ? "empty" : "***")
              << ", catalogOnRegister=" << (catalogOnRegisterEnabled ? "on" : "off")
              << ", catalogCooldownSec=" << catalogOnRegisterCooldownSec;
    }
    
    // 刷新配置（修改配置后调用）
    void reload() {
        InfoL << "【SystemConfigCache】Reloading from DB...";
        loadFromDb();
    }
    
    // 辅助函数：清理字符串
    std::string trimString(const std::string& s) {
        if (s.empty()) return "";
        size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        size_t end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }
};

static SystemConfigCache g_systemConfig;
struct RecordInfoRelaySession {
    int64_t upstreamDbId{0};
    int upstreamSn{0};
    int downstreamSn{0};
    std::string upstreamCatalogDeviceId;
    std::string downstreamDeviceGbId;
    std::string upstreamSignalIp;
    int upstreamSignalPort{5060};
    std::string upstreamPeerGbId;
    std::chrono::steady_clock::time_point deadline{};
};
static std::mutex g_recordRelayMutex;
static std::unordered_map<int, RecordInfoRelaySession> g_recordRelayByDownstreamSn;
static std::atomic<int> g_recordRelaySn{900000000};

static bool replaceFirstXmlTag(std::string& xml, const std::string& tagName, const std::string& value) {
    const std::string openTag = "<" + tagName + ">";
    const std::string closeTag = "</" + tagName + ">";
    size_t s = xml.find(openTag);
    if (s == std::string::npos) return false;
    s += openTag.size();
    size_t e = xml.find(closeTag, s);
    if (e == std::string::npos || e < s) return false;
    xml.replace(s, e - s, value);
    return true;
}

static int replaceAllXmlDeviceId(std::string& xml, const std::string& fromValue, const std::string& toValue) {
    if (fromValue.empty() || fromValue == toValue) return 0;
    const std::string fromTag = "<DeviceID>" + fromValue + "</DeviceID>";
    const std::string toTag = "<DeviceID>" + toValue + "</DeviceID>";
    size_t pos = 0;
    int cnt = 0;
    while (true) {
        size_t p = xml.find(fromTag, pos);
        if (p == std::string::npos) break;
        xml.replace(p, fromTag.size(), toTag);
        pos = p + toTag.size();
        cnt++;
    }
    return cnt;
}

/**
 * @brief 从 XML 字符串提取标签内容
 * @param xml XML 字符串
 * @param tagName 标签名
 * @return 标签内容，未找到返回空字符串
 */
std::string extractXmlTag(const std::string& xml, const std::string& tagName) {
    std::string startTag = "<" + tagName + ">";
    std::string endTag = "</" + tagName + ">";
    size_t start = xml.find(startTag);
    if (start == std::string::npos) return "";
    start += startTag.length();
    size_t end = xml.find(endTag, start);
    if (end == std::string::npos) return "";
    return xml.substr(start, end - start);
}

/**
 * @brief 从 SIP URI 提取设备 ID
 * @param uri SIP URI 指针
 * @return 提取的设备 ID，失败返回空字符串
 * @details 解析格式：sip:DeviceID@host，提取 DeviceID 部分
 */
std::string parseDeviceIdFromUri(pjsip_uri* uri) {
    if (!uri) return "";

    // 将 URI 打印成字符串，然后解析 sip:DeviceID@host
    char buf[256] = {0};
    int len = pjsip_uri_print(PJSIP_URI_IN_REQ_URI, uri, buf, sizeof(buf));
    if (len <= 0) {
        return "";
    }
    std::string s(buf, len);

    size_t start = s.find(':');
    if (start == std::string::npos) start = 0;
    else start++;
    size_t end = s.find('@', start);
    if (end == std::string::npos) return s.substr(start);
    return s.substr(start, end - start);
}

/**
 * @brief 解析上级国标 ID（优先 Contact，其次 From）
 * @details 针对“上级发送到本机”的请求，先取 Contact URI user；缺失再取 From URI user。
 */
static std::string extractPeerGbIdContactFirst(pjsip_msg* msg) {
    if (!msg) return "";
    pjsip_contact_hdr* contact = (pjsip_contact_hdr*)pjsip_msg_find_hdr(msg, PJSIP_H_CONTACT, nullptr);
    if (contact && contact->uri) {
        std::string gb = parseDeviceIdFromUri(contact->uri);
        if (!gb.empty()) return gb;
    }
    pjsip_from_hdr* from = (pjsip_from_hdr*)pjsip_msg_find_hdr(msg, PJSIP_H_FROM, nullptr);
    if (from && from->uri) {
        return parseDeviceIdFromUri(from->uri);
    }
    return "";
}

/**
 * @brief 解析“上级平台匹配结果”：优先 gb_id（Contact->From），再回退信令源地址
 */
static gb::UpstreamSignalMatch resolveUpstreamMatchForRequest(pjsip_rx_data* rdata, pjsip_msg* msg,
                                                              std::string* outPeerGbId = nullptr) {
    gb::UpstreamSignalMatch m;
    std::string peerGbId = extractPeerGbIdContactFirst(msg);
    if (outPeerGbId) *outPeerGbId = peerGbId;
    if (!peerGbId.empty()) {
        m = gb::matchUpstreamByGbId(peerGbId.c_str());
        if (m.matched) return m;
    }
    std::string sigIp;
    int sigPort = 0;
    extractSignalSrcFromRdata(rdata, sigIp, sigPort);
    return gb::matchUpstreamBySignalSource(sigIp.c_str(), sigPort);
}

static void cleanupExpiredRecordRelaySessions() {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(g_recordRelayMutex);
    for (auto it = g_recordRelayByDownstreamSn.begin(); it != g_recordRelayByDownstreamSn.end();) {
        if (it->second.deadline.time_since_epoch().count() != 0 && now > it->second.deadline) {
            WarnL << "【RecordInfoRelay】会话超时丢弃 downstreamSN=" << it->first
                  << " upstreamDbId=" << it->second.upstreamDbId << " upstreamSN=" << it->second.upstreamSn;
            it = g_recordRelayByDownstreamSn.erase(it);
        } else {
            ++it;
        }
    }
}

static bool relayRecordInfoResponseToUpstream(const std::string& body) {
    int downstreamSn = 0;
    std::string snStr = extractXmlTag(body, "SN");
    if (!snStr.empty()) downstreamSn = std::atoi(snStr.c_str());
    if (downstreamSn <= 0) return false;
    RecordInfoRelaySession sess;
    {
        std::lock_guard<std::mutex> lk(g_recordRelayMutex);
        auto it = g_recordRelayByDownstreamSn.find(downstreamSn);
        if (it == g_recordRelayByDownstreamSn.end()) return false;
        sess = it->second;
        g_recordRelayByDownstreamSn.erase(it);
    }

    std::string outXml = body;
    replaceFirstXmlTag(outXml, "SN", std::to_string(sess.upstreamSn));
    replaceFirstXmlTag(outXml, "DeviceID", sess.upstreamCatalogDeviceId);
    replaceAllXmlDeviceId(outXml, sess.downstreamDeviceGbId, sess.upstreamCatalogDeviceId);

    bool ok = gb::sendUpstreamManscdpMessage(sess.upstreamSignalIp, sess.upstreamSignalPort, sess.upstreamPeerGbId,
                                             sess.upstreamSignalIp, sess.upstreamSignalPort, outXml, "MESSAGE");
    if (!ok) {
        ErrorL << "【RecordInfoRelay】回传上级失败 upstreamDbId=" << sess.upstreamDbId
               << " upstreamSN=" << sess.upstreamSn << " downstreamSN=" << downstreamSn;
        return true;
    }
    InfoL << "【RecordInfoRelay】已回传上级 upstreamDbId=" << sess.upstreamDbId
          << " upstreamSN=" << sess.upstreamSn << " downstreamSN=" << downstreamSn;
    return true;
}

static std::string buildEmptyRecordInfoResponseXml(int sn, const std::string& deviceId) {
    std::ostringstream xml;
    xml << "<?xml version=\"1.0\"?>\r\n"
        << "<Response>\r\n"
        << "<CmdType>RecordInfo</CmdType>\r\n"
        << "<SN>" << sn << "</SN>\r\n"
        << "<DeviceID>" << deviceId << "</DeviceID>\r\n"
        << "<Name></Name>\r\n"
        << "<SumNum>0</SumNum>\r\n"
        << "<RecordList Num=\"0\"></RecordList>\r\n"
        << "</Response>\r\n";
    return xml.str();
}

static void sendEmptyRecordInfoResponseToUpstream(const gb::UpstreamSignalMatch& umMsg, const std::string& upstreamPeerGbId,
                                                  const std::string& signalIp, int signalPort, int upstreamSn,
                                                  const std::string& queryDeviceId, const std::string& reason) {
    if (!umMsg.matched || !umMsg.enabled || upstreamPeerGbId.empty() || signalIp.empty() || signalPort <= 0 ||
        upstreamSn <= 0 || queryDeviceId.empty()) {
        return;
    }
    std::string xml = buildEmptyRecordInfoResponseXml(upstreamSn, queryDeviceId);
    bool ok = gb::sendUpstreamManscdpMessage(signalIp, signalPort, upstreamPeerGbId, signalIp, signalPort, xml, "MESSAGE");
    if (!ok) {
        ErrorL << "【RecordInfoRelay】回空应答失败 upstreamId=" << umMsg.platformDbId << " SN=" << upstreamSn
               << " reason=" << reason;
    } else {
        WarnL << "【RecordInfoRelay】回空应答 upstreamId=" << umMsg.platformDbId << " SN=" << upstreamSn
              << " reason=" << reason;
    }
}

static bool handleUpstreamPtzDeviceControl(const gb::UpstreamSignalMatch& umMsg, const std::string& body) {
    if (!umMsg.matched || !umMsg.enabled) return false;
    const std::string catalogDeviceId = extractXmlTag(body, "DeviceID");
    const std::string ptzCmdHex = extractXmlTag(body, "PTZCmd");
    if (catalogDeviceId.empty() || ptzCmdHex.empty()) {
        WarnL << "【UpstreamPTZ】缺少 DeviceID 或 PTZCmd upstreamId=" << umMsg.platformDbId;
        return true;
    }
    std::string downstreamDeviceGbId;
    std::string downstreamPlatformGbId;
    long long cameraDbId = 0;
    if (!gb::resolveUpstreamCatalogDeviceId(umMsg.platformDbId, catalogDeviceId, downstreamDeviceGbId,
                                            downstreamPlatformGbId, cameraDbId)) {
        WarnL << "【UpstreamPTZ】目录ID映射失败 upstreamId=" << umMsg.platformDbId
              << " catalogDeviceId=" << catalogDeviceId;
        return true;
    }
    bool ok = gb::enqueuePtzDeviceControlHex(downstreamPlatformGbId, downstreamDeviceGbId, ptzCmdHex);
    if (!ok) {
        WarnL << "【UpstreamPTZ】下发入队失败 upstreamId=" << umMsg.platformDbId
              << " downPlatform=" << downstreamPlatformGbId << " downDevice=" << downstreamDeviceGbId;
        return true;
    }
    InfoL << "【UpstreamPTZ】已入队下发 upstreamId=" << umMsg.platformDbId
          << " catalogDeviceId=" << catalogDeviceId << " downDevice=" << downstreamDeviceGbId;
    return true;
}

static bool handleUpstreamRecordInfoQuery(const gb::UpstreamSignalMatch& umMsg, const std::string& upstreamPeerGbId,
                                          const std::string& body, const std::string& signalIp, int signalPort) {
    if (!umMsg.matched || !umMsg.enabled || upstreamPeerGbId.empty() || signalIp.empty() || signalPort <= 0) return false;
    const std::string upstreamCatalogDeviceId = extractXmlTag(body, "DeviceID");
    const std::string startTime = extractXmlTag(body, "StartTime");
    const std::string endTime = extractXmlTag(body, "EndTime");
    const std::string snStr = extractXmlTag(body, "SN");
    int upstreamSn = snStr.empty() ? 0 : std::atoi(snStr.c_str());
    if (upstreamCatalogDeviceId.empty() || upstreamSn <= 0) {
        WarnL << "【RecordInfoRelay】上级查询缺少 DeviceID 或 SN upstreamId=" << umMsg.platformDbId;
        return true;
    }

    std::string downstreamDeviceGbId;
    std::string downstreamPlatformGbId;
    long long cameraDbId = 0;
    if (!gb::resolveUpstreamCatalogDeviceId(umMsg.platformDbId, upstreamCatalogDeviceId, downstreamDeviceGbId,
                                            downstreamPlatformGbId, cameraDbId)) {
        WarnL << "【RecordInfoRelay】未找到上级目录ID映射 upstreamId=" << umMsg.platformDbId
              << " queryDeviceID=" << upstreamCatalogDeviceId;
        sendEmptyRecordInfoResponseToUpstream(umMsg, upstreamPeerGbId, signalIp, signalPort, upstreamSn,
                                              upstreamCatalogDeviceId, "mapping-not-found");
        return true;
    }

    gb::upstreamBridgeMarkReplayHint(umMsg.platformDbId, upstreamCatalogDeviceId, 45);

    int downstreamSn = g_recordRelaySn.fetch_add(1);
    if (downstreamSn <= 0) downstreamSn = std::abs(downstreamSn) + 1000;
    RecordInfoRelaySession sess;
    sess.upstreamDbId = umMsg.platformDbId;
    sess.upstreamSn = upstreamSn;
    sess.downstreamSn = downstreamSn;
    sess.upstreamCatalogDeviceId = upstreamCatalogDeviceId;
    sess.downstreamDeviceGbId = downstreamDeviceGbId;
    sess.upstreamSignalIp = signalIp;
    sess.upstreamSignalPort = signalPort;
    sess.upstreamPeerGbId = upstreamPeerGbId;
    sess.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);

    if (!gb::enqueueRecordInfoQuery(downstreamPlatformGbId, downstreamDeviceGbId, startTime, endTime, downstreamSn)) {
        ErrorL << "【RecordInfoRelay】入队下级查询失败 upstreamId=" << umMsg.platformDbId
               << " platformGbId=" << downstreamPlatformGbId << " deviceGbId=" << downstreamDeviceGbId;
        sendEmptyRecordInfoResponseToUpstream(umMsg, upstreamPeerGbId, signalIp, signalPort, upstreamSn,
                                              upstreamCatalogDeviceId, "enqueue-failed");
        return true;
    }
    {
        std::lock_guard<std::mutex> lk(g_recordRelayMutex);
        g_recordRelayByDownstreamSn[downstreamSn] = sess;
    }
    InfoL << "【RecordInfoRelay】已转发下级查询 upstreamId=" << umMsg.platformDbId << " upstreamSN=" << upstreamSn
          << " downstreamSN=" << downstreamSn << " downPlatform=" << downstreamPlatformGbId
          << " downDevice=" << downstreamDeviceGbId;
    return true;
}

/**
 * @brief 发送 401 鉴权响应
 * @param rdata PJSIP 接收数据
 * @param realm 域（realm）值
 * @details 构建包含 WWW-Authenticate 头的 401 响应，
 *          触发客户端发送带鉴权信息的请求
 */
void send401Digest(pjsip_rx_data* rdata, const char* realm) {
    pjsip_tx_data* tdata = nullptr;
    pj_status_t st = pjsip_endpt_create_response(g_sip_endpt, rdata, 401, nullptr, &tdata);
    if (st != PJ_SUCCESS) return;

    // 生成随机 nonce
    char nonce[33], opaque[33];
    snprintf(nonce, sizeof(nonce), "%08x%08x%08x%08x",
        rand(), rand(), rand(), rand());
    snprintf(opaque, sizeof(opaque), "%08x%08x%08x%08x",
        rand(), rand(), rand(), rand());

    // 构建 WWW-Authenticate 头
    char authHdr[512];
    snprintf(authHdr, sizeof(authHdr),
        "Digest realm=\"%s\",nonce=\"%s\",opaque=\"%s\",algorithm=MD5",
        realm, nonce, opaque);

    pj_str_t authName = pj_str((char*)"WWW-Authenticate");
    pj_str_t authValue = pj_str(authHdr);
    pjsip_generic_string_hdr* hdr = pjsip_generic_string_hdr_create(tdata->pool, &authName, &authValue);
    pjsip_msg_add_hdr(tdata->msg, (pjsip_hdr*)hdr);

    pjsip_endpt_send_response2(g_sip_endpt, rdata, tdata, nullptr, nullptr);
}

/**
 * @brief 发送简单 SIP 响应
 * @param rdata PJSIP 接收数据
 * @param code HTTP 状态码（200、403等）
 * @details 发送无自定义头的简单响应
 */
void sendSimpleResponse(pjsip_rx_data* rdata, int code) {
    // RFC3261：ACK 无对应 SIP 响应；PJSIP 对 ACK 调用 create_response 会 assert 崩溃
    if (rdata && rdata->msg_info.msg && rdata->msg_info.msg->type == PJSIP_REQUEST_MSG &&
        rdata->msg_info.msg->line.req.method.id == PJSIP_ACK_METHOD) {
        WarnL << "【SIP】sendSimpleResponse 忽略 ACK（不应生成响应）";
        return;
    }
    pjsip_tx_data* tdata = nullptr;
    pj_status_t st = pjsip_endpt_create_response(g_sip_endpt, rdata, code, nullptr, &tdata);
    if (st == PJ_SUCCESS && tdata) {
        pjsip_endpt_send_response2(g_sip_endpt, rdata, tdata, nullptr, nullptr);
    }
}

/**
 * @brief 200 OK + Application/MANSCDP+xml 正文（上级目录查询应答等）
 */
static void sendResponseWithManscdpBody(pjsip_rx_data* rdata, const std::string& xml) {
    pjsip_tx_data* tdata = nullptr;
    pj_status_t st = pjsip_endpt_create_response(g_sip_endpt, rdata, 200, nullptr, &tdata);
    if (st != PJ_SUCCESS || !tdata) {
        sendSimpleResponse(rdata, 500);
        return;
    }
    pj_pool_t* pool = tdata->pool;
    pj_str_t type_str = pj_str((char*)"Application");
    pj_str_t subtype_str = pj_str((char*)"MANSCDP+xml");
    char* buf = (char*)pj_pool_alloc(pool, xml.size() + 1);
    if (!buf) {
        sendSimpleResponse(rdata, 500);
        return;
    }
    memcpy(buf, xml.c_str(), xml.size());
    buf[xml.size()] = '\0';
    pj_str_t body_str;
    body_str.ptr = buf;
    body_str.slen = (int)xml.size();
    tdata->msg->body = pjsip_msg_body_create(pool, &type_str, &subtype_str, &body_str);
    pjsip_endpt_send_response2(g_sip_endpt, rdata, tdata, nullptr, nullptr);
}

/**
 * @brief 发送注册成功响应（200 OK）
 * @param rdata PJSIP 接收数据
 * @param expires Expires 值（秒），告诉设备注册有效期
 * @details 发送 200 OK 响应，包含 Expires 头和 Date 头
 *          符合 GB28181-2016 规范，确保设备知道注册有效期
 */
void sendRegisterSuccessResponse(pjsip_rx_data* rdata, int expires) {
    pjsip_tx_data* tdata = nullptr;
    pj_status_t st = pjsip_endpt_create_response(g_sip_endpt, rdata, 200, nullptr, &tdata);
    if (st != PJ_SUCCESS || !tdata) {
        ErrorL << "Failed to create 200 OK response for REGISTER";
        return;
    }
    
    // 注意：与 1.107 平台保持一致，不添加 Contact 头
    // 某些设备可能不喜欢 200 OK 中有 Contact 头
    
    // 添加 Expires 头
    pjsip_expires_hdr* expHdr = pjsip_expires_hdr_create(tdata->pool, expires);
    if (expHdr) {
        pjsip_msg_add_hdr(tdata->msg, (pjsip_hdr*)expHdr);
    }
    
    // 添加 Server 头
    pj_str_t srvName = pj_str((char*)"Server");
    pj_str_t srvValue = pj_str((char*)"GBService");
    pjsip_generic_string_hdr* srvHdr = pjsip_generic_string_hdr_create(tdata->pool, &srvName, &srvValue);
    if (srvHdr) {
        pjsip_msg_add_hdr(tdata->msg, (pjsip_hdr*)srvHdr);
    }
    
    InfoL << "【REGISTER 200 OK】Expires=" << expires << "s, Server=GBService";
    
    pjsip_endpt_send_response2(g_sip_endpt, rdata, tdata, nullptr, nullptr);
}

/**
 * @brief 处理 REGISTER 注册请求
 * @param rdata PJSIP 接收数据
 * @details 处理流程：
 *          1. 提取设备 ID（从 From 头，GB28181 规范）
 *          2. 提取 Expires 头
 *          3. 提取 Authorization 头
 *          4. 调用 SipHandler::handleRegister 处理业务逻辑
 *          5. 根据返回码发送响应（200、401、403）
 * @note 重要：GB28181 注册请求中，Request-URI 是目标服务器ID，
 *       真实设备ID在 From 头中，必须从 From 头提取
 */
void handleRegisterRequest(pjsip_rx_data* rdata) {
    pjsip_msg* msg = rdata->msg_info.msg;

    {
        std::string sigIp;
        int sigPort = 0;
        extractSignalSrcFromRdata(rdata, sigIp, sigPort);
        gb::UpstreamSignalMatch um = gb::matchUpstreamBySignalSource(sigIp.c_str(), sigPort);
        if (um.matched && !um.enabled) {
            sendSimpleResponse(rdata, 403);
            return;
        }
    }

    if (rdata->pkt_info.packet && rdata->pkt_info.len > 0) {
        size_t previewLen = std::min<size_t>(static_cast<size_t>(rdata->pkt_info.len), 800);
        InfoL << "【REGISTER RAW PREVIEW】\n"
              << std::string(rdata->pkt_info.packet, previewLen);
    }

    // 提取设备ID（从 From 头，符合 GB28181 规范）
    std::string deviceId;
    pjsip_from_hdr* fromHdr = (pjsip_from_hdr*)pjsip_msg_find_hdr(msg, PJSIP_H_FROM, nullptr);
    if (fromHdr && fromHdr->uri) {
        deviceId = parseDeviceIdFromUri(fromHdr->uri);
    }
    // 如果 From 头没有，尝试 Contact 头作为后备
    if (deviceId.empty()) {
        pjsip_contact_hdr* contactHdr = (pjsip_contact_hdr*)pjsip_msg_find_hdr(msg, PJSIP_H_CONTACT, nullptr);
        if (contactHdr && contactHdr->uri) {
            deviceId = parseDeviceIdFromUri(contactHdr->uri);
        }
    }
    if (deviceId.empty()) {
        sendSimpleResponse(rdata, 400);
        return;
    }

    // 提取 Expires
    int expires = 3600;
    pjsip_expires_hdr* expHdr = (pjsip_expires_hdr*)pjsip_msg_find_hdr(msg, PJSIP_H_EXPIRES, nullptr);
    if (expHdr) expires = expHdr->ivalue;

    // 提取 Authorization（完整 Digest 头文本，供自定义解析使用）
    std::string authHeader;
    pjsip_authorization_hdr* auth = (pjsip_authorization_hdr*)pjsip_msg_find_hdr(msg, PJSIP_H_AUTHORIZATION, nullptr);
    if (auth) {
        char buf[512] = {0};
        // 打印整条 Authorization 头，例如：Authorization: Digest username="...", realm="...", ...
        int len = pjsip_hdr_print_on((pjsip_hdr*)auth, buf, sizeof(buf));
        if (len > 0) {
            std::string h(buf, len);
            // 只保留以 "Digest" 开头的部分，去掉前面的 "Authorization:" 前缀
            size_t pos = h.find("Digest");
            if (pos != std::string::npos) {
                authHeader = h.substr(pos);
            } else {
                authHeader = h; // 退化处理
            }
        }
    }

    // 提取 Contact 头中的地址信息（IP 和端口）
    std::string contactIp;
    int contactPort = 0;
    pjsip_contact_hdr* contactHdr = (pjsip_contact_hdr*)pjsip_msg_find_hdr(msg, PJSIP_H_CONTACT, nullptr);
    if (contactHdr && contactHdr->uri) {
        // 将 Contact URI 打印为字符串
        char buf[256] = {0};
        int len = pjsip_uri_print(PJSIP_URI_IN_CONTACT_HDR, contactHdr->uri, buf, sizeof(buf));
        if (len > 0) {
            std::string contactUri(buf, len);
            // 解析 sip:user@host:port 格式，提取 host 和 port
            size_t atPos = contactUri.find('@');
            if (atPos != std::string::npos) {
                size_t hostStart = atPos + 1;
                size_t colonPos = contactUri.find(':', hostStart);
                if (colonPos != std::string::npos) {
                    contactIp = contactUri.substr(hostStart, colonPos - hostStart);
                    contactPort = std::atoi(contactUri.substr(colonPos + 1).c_str());
                } else {
                    // 没有端口，使用默认 5060
                    contactIp = contactUri.substr(hostStart);
                    contactPort = 5060;
                }
            }
        }
    }

    std::string signalSrcIp;
    int signalSrcPort = 0;
    extractSignalSrcFromRdata(rdata, signalSrcIp, signalSrcPort);

    InfoL << "【REGISTER PARSED】deviceId=" << deviceId
          << " expires=" << expires
          << " auth=" << (authHeader.empty() ? "none" : "digest")
          << " contactIp=" << (contactIp.empty() ? "(empty)" : contactIp)
          << " contactPort=" << contactPort
          << " signalSrc=" << (signalSrcIp.empty() ? "(empty)" : signalSrcIp) << ":" << signalSrcPort;

    // 调用业务处理
    std::string method = "REGISTER";
    // 将请求 URI 打印为字符串，供鉴权使用
    std::string uri;
    if (msg->line.req.uri) {
        char buf[256] = {0};
        int len = pjsip_uri_print(PJSIP_URI_IN_REQ_URI, msg->line.req.uri, buf, sizeof(buf));
        if (len > 0) {
            uri.assign(buf, len);
        }
    }

    int ret = handleRegister(deviceId, expires, authHeader, method, uri, contactIp, contactPort,
                             signalSrcIp, signalSrcPort);

    if (ret == 401) {
        // 使用预加载的系统配置 realm（高性能，避免每次读数据库）
        if (!g_systemConfig.loaded) {
            g_systemConfig.loadFromDb();  // 首次加载
        }
        send401Digest(rdata, g_systemConfig.realm.c_str());
    } else if (ret == 403) {
        sendSimpleResponse(rdata, 403);
    } else {
        if (signalSrcIp.empty() || signalSrcPort <= 0) {
            WarnL << "【REGISTER】未解析到 UDP/TCP 源地址，signal_src 字段可能为空；"
                     "若库中 last_heartbeat_at 仍为空请确认已重启新版服务且迁移已执行。deviceId="
                  << deviceId;
        }
        // 注册成功，发送 200 OK 并包含 Expires 头
        sendRegisterSuccessResponse(rdata, expires);
    }
}

/**
 * @brief 处理 MESSAGE 请求（Keepalive、Catalog）
 * @param rdata PJSIP 接收数据
 * @details 处理 GB28181 协议中的 MESSAGE 消息：
 *          - Keepalive：心跳消息，更新设备在线状态
 *          - Catalog Notify/Response：设备目录上报
 *          自动识别消息类型并调用相应处理函数
 */
void handleMessageRequest(pjsip_rx_data* rdata) {
    pjsip_msg* msg = rdata->msg_info.msg;

    // 提取设备ID - 先从 URI 取
    std::string deviceId;
    if (msg->line.req.uri) {
        deviceId = parseDeviceIdFromUri(msg->line.req.uri);
    }
    if (deviceId.empty()) {
        pjsip_from_hdr* from = (pjsip_from_hdr*)pjsip_msg_find_hdr(msg, PJSIP_H_FROM, nullptr);
        if (from && from->uri) {
            deviceId = parseDeviceIdFromUri(from->uri);
        }
    }

    // 提取 body
    std::string body;
    if (msg->body && msg->body->data) {
        body = std::string((char*)msg->body->data, msg->body->len);
    }

    // ===== 详细日志：打印所有收到的 MESSAGE =====
    InfoL << "【RECEIVED MESSAGE】From: " << deviceId << " | BodySize: " << body.size();
    InfoL << "【BODY PREVIEW】" << body.substr(0, 300);  // 打印前300字符

    std::string upstreamPeerGbId;
    gb::UpstreamSignalMatch umMsg = resolveUpstreamMatchForRequest(rdata, msg, &upstreamPeerGbId);
    if (umMsg.matched && !umMsg.enabled) {
        WarnL << "【MESSAGE】403 拒绝: peerGbId=" << (upstreamPeerGbId.empty() ? "(empty)" : upstreamPeerGbId)
              << " platformDbId=" << umMsg.platformDbId;
        sendSimpleResponse(rdata, 403);
        return;
    }
    const bool upstreamPeerOk = umMsg.matched && umMsg.enabled;

    // 检查 Content-Type
    pjsip_ctype_hdr* ct = (pjsip_ctype_hdr*)pjsip_msg_find_hdr(msg, PJSIP_H_CONTENT_TYPE, nullptr);
    if (ct) {
        std::string type(ct->media.type.ptr, ct->media.type.slen);
        std::string subtype(ct->media.subtype.ptr, ct->media.subtype.slen);
        InfoL << "【CONTENT-TYPE】" << type << "/" << subtype;
    }

    // 检查是否是 Catalog 消息（支持多种格式）
    // 1. <CmdType>Notify</CmdType> - 主动上报
    // 2. <CmdType>Catalog</CmdType> + <Response> - 查询响应  
    // 3. <CmdType>Catalog</CmdType> + <Query> - 查询请求
    bool hasNotify = (body.find("<CmdType>Notify</CmdType>") != std::string::npos);
    bool hasCatalog = (body.find("<CmdType>Catalog</CmdType>") != std::string::npos);
    bool hasResponse = (body.find("<Response>") != std::string::npos);
    bool hasQuery = (body.find("<Query>") != std::string::npos);
    bool hasKeepalive = (body.find("<CmdType>Keepalive</CmdType>") != std::string::npos);

    InfoL << "【CMDTYPE CHECK】Notify=" << hasNotify << " Catalog=" << hasCatalog 
          << " Response=" << hasResponse << " Query=" << hasQuery << " Keepalive=" << hasKeepalive;
    cleanupExpiredRecordRelaySessions();

    bool hasRecordInfo = (body.find("<CmdType>RecordInfo</CmdType>") != std::string::npos);
    bool hasDeviceControl = (body.find("<CmdType>DeviceControl</CmdType>") != std::string::npos);
    bool hasPtzCmd = (body.find("<PTZCmd>") != std::string::npos);
    if (hasRecordInfo && hasResponse) {
        if (relayRecordInfoResponseToUpstream(body)) {
            sendSimpleResponse(rdata, 200);
            return;
        }
        InfoL << "【RECORDINFO RESPONSE】From " << deviceId;
        gb::handleRecordInfoMessage(deviceId, body);
        sendSimpleResponse(rdata, 200);
        return;
    }
    if (hasRecordInfo && hasQuery && upstreamPeerOk) {
        std::string signalIp;
        int signalPort = 0;
        extractSignalSrcFromRdata(rdata, signalIp, signalPort);
        sendSimpleResponse(rdata, 200);  // 先回 SIP 200，业务应答异步转发处理
        (void)handleUpstreamRecordInfoQuery(umMsg, upstreamPeerGbId, body, signalIp, signalPort);
        return;
    }
    if (upstreamPeerOk && hasDeviceControl && hasPtzCmd) {
        sendSimpleResponse(rdata, 200);  // 先回 SIP 200，云台控制异步下发
        (void)handleUpstreamPtzDeviceControl(umMsg, body);
        return;
    }

    bool isCatalogNotify = hasNotify;
    bool isCatalogResponse = hasCatalog && hasResponse;
    bool isCatalogQuery = hasCatalog && hasQuery;

    if (upstreamPeerOk && isCatalogQuery && !hasResponse) {
        std::string qDev = extractXmlTag(body, "DeviceID");
        std::string snStrU = extractXmlTag(body, "SN");
        int snU = snStrU.empty() ? 0 : std::atoi(snStrU.c_str());
        // GB28181：先回 200 OK 确认收到查询，再用 MESSAGE 分包发送应答
        sendSimpleResponse(rdata, 200);
        // 异步排队发送目录应答（复用主动推送的分包逻辑 + MESSAGE 方法）
        gb::enqueueUpstreamCatalogQueryResponse(umMsg.platformDbId, snU);
        InfoL << "【上级Catalog查询】upstreamId=" << umMsg.platformDbId << " SN=" << snU << " 已回200并入队应答";
        return;
    }

    if (upstreamPeerOk && (isCatalogNotify || isCatalogResponse || isCatalogQuery)) {
        InfoL << "【上级Catalog类MESSAGE】忽略下级目录入库 upstreamId=" << umMsg.platformDbId;
        sendSimpleResponse(rdata, 200);
        return;
    }

    if (isCatalogNotify || isCatalogResponse || isCatalogQuery) {
        InfoL << "【CATALOG DETECTED】Processing Catalog from " << deviceId;

        // 从 XML 中提取 SN（命令序列号）
        std::string snStr = extractXmlTag(body, "SN");
        int sn = 0;
        if (!snStr.empty()) {
            sn = std::atoi(snStr.c_str());
        }
        
        // 从 XML 中提取 SumNum（总数）
        std::string sumNumStr = extractXmlTag(body, "SumNum");
        int sumNum = 0;
        if (!sumNumStr.empty()) {
            sumNum = std::atoi(sumNumStr.c_str());
        }
        
        // 从 XML 中提取 DeviceList Num（当前消息包含的设备数）
        // DeviceList Num="X" 格式，需要提取属性值
        size_t deviceListPos = body.find("<DeviceList");
        int deviceListNum = 0;
        if (deviceListPos != std::string::npos) {
            size_t numAttrPos = body.find("Num=\"", deviceListPos);
            if (numAttrPos != std::string::npos) {
                size_t numValueStart = numAttrPos + 5;  // 跳过 Num="
                size_t numValueEnd = body.find("\"", numValueStart);
                if (numValueEnd != std::string::npos) {
                    std::string numStr = body.substr(numValueStart, numValueEnd - numValueStart);
                    deviceListNum = std::atoi(numStr.c_str());
                }
            }
        }
        
        // 提取所有 Item 的 Index
        std::string itemIndices;
        size_t pos = 0;
        while (true) {
            size_t itemStart = body.find("<Item", pos);
            if (itemStart == std::string::npos) break;
            
            size_t indexAttrPos = body.find("Index=\"", itemStart);
            if (indexAttrPos != std::string::npos) {
                size_t indexValueStart = indexAttrPos + 7;  // 跳过 Index="
                size_t indexValueEnd = body.find("\"", indexValueStart);
                if (indexValueEnd != std::string::npos) {
                    std::string indexStr = body.substr(indexValueStart, indexValueEnd - indexValueStart);
                    if (!itemIndices.empty()) itemIndices += ",";
                    itemIndices += indexStr;
                }
            }
            
            // 移动到下一个位置
            size_t itemEnd = body.find("</Item>", itemStart);
            if (itemEnd == std::string::npos) break;
            pos = itemEnd + 7;
        }
        
        InfoL << "【CATALOG PAGINATION】SN=" << sn 
              << " SumNum=" << sumNum 
              << " DeviceListNum=" << deviceListNum
              << " ItemIndices=" << (itemIndices.empty() ? "(none)" : itemIndices);
        
        // 调用新的目录树处理函数（支持分页和设备/目录/行政区域）
        handleCatalogTreeNotify(deviceId, body, sn);

        // 发送 200 OK 响应
        sendSimpleResponse(rdata, 200);
        InfoL << "【CATALOG RESPONSE】200 OK sent to " << deviceId << " SN=" << sn;
        return;
    }

    // 检查是否是 Keepalive
    if (hasKeepalive) {
        // 心跳消息的设备ID在 XML body 中的 <DeviceID> 标签，而不是 From 头
        std::string keepaliveDeviceId = extractXmlTag(body, "DeviceID");
        if (!keepaliveDeviceId.empty()) {
            deviceId = keepaliveDeviceId;  // 使用 body 中的 DeviceID
        }
        InfoL << "【KEEPALIVE】From: " << deviceId;

        std::string signalSrcIp;
        int signalSrcPort = 0;
        extractSignalSrcFromRdata(rdata, signalSrcIp, signalSrcPort);
        handleKeepalive(deviceId, signalSrcIp, signalSrcPort);

        // 发送 200 OK 响应
        sendSimpleResponse(rdata, 200);
        return;
    }

    if (upstreamPeerOk) {
        InfoL << "【上级MESSAGE】未细分 CmdType，回 200 From=" << deviceId;
        sendSimpleResponse(rdata, 200);
        return;
    }

    // 其他 MESSAGE 类型，也返回 200
    InfoL << "【UNKNOWN MESSAGE】From: " << deviceId << " -> 200";
    sendSimpleResponse(rdata, 200);
}

/**
 * @brief 处理 NOTIFY 请求（GB28181 设备目录主动上报）
 * @param rdata PJSIP 接收数据
 * @details 处理设备主动上报的目录信息（Catalog Notify）
 *          与 MESSAGE 处理类似，但使用不同的 SIP 方法
 */
void handleNotifyRequest(pjsip_rx_data* rdata) {
    pjsip_msg* msg = rdata->msg_info.msg;

    {
        std::string peerGbId;
        gb::UpstreamSignalMatch um = resolveUpstreamMatchForRequest(rdata, msg, &peerGbId);
        if (um.matched && !um.enabled) {
            WarnL << "【NOTIFY】403 拒绝: peerGbId=" << (peerGbId.empty() ? "(empty)" : peerGbId)
                  << " platformDbId=" << um.platformDbId;
            sendSimpleResponse(rdata, 403);
            return;
        }
        if (um.matched && um.enabled) {
            sendSimpleResponse(rdata, 200);
            return;
        }
    }

    // 提取设备ID - 先从 URI 取
    std::string deviceId;
    if (msg->line.req.uri) {
        deviceId = parseDeviceIdFromUri(msg->line.req.uri);
    }
    if (deviceId.empty()) {
        pjsip_from_hdr* from = (pjsip_from_hdr*)pjsip_msg_find_hdr(msg, PJSIP_H_FROM, nullptr);
        if (from && from->uri) {
            deviceId = parseDeviceIdFromUri(from->uri);
        }
    }

    // 提取 body
    std::string body;
    if (msg->body && msg->body->data) {
        body = std::string((char*)msg->body->data, msg->body->len);
    }

    // 详细日志
    InfoL << "【RECEIVED NOTIFY】From: " << deviceId << " | BodySize: " << body.size();
    InfoL << "【BODY PREVIEW】" << body.substr(0, 400);

    // 检查是否是 Catalog
    bool hasCatalog = (body.find("<CmdType>Catalog</CmdType>") != std::string::npos);
    bool hasResponse = (body.find("<Response>") != std::string::npos);
    bool hasNotify = (body.find("<CmdType>Notify</CmdType>") != std::string::npos);

    InfoL << "【NOTIFY CMDTYPE】Catalog=" << hasCatalog << " Response=" << hasResponse << " Notify=" << hasNotify;

    if (hasCatalog || hasNotify) {
        InfoL << "【CATALOG NOTIFY DETECTED】Processing...";
        
        // 从 XML 中提取 SN（命令序列号）用于分页匹配
        std::string snStr = extractXmlTag(body, "SN");
        int sn = 0;
        if (!snStr.empty()) {
            sn = std::atoi(snStr.c_str());
        }
        
        // 调用新版目录树处理函数（支持分页和设备/目录/行政区域）
        handleCatalogTreeNotify(deviceId, body, sn);
        
        InfoL << "【CATALOG NOTIFY】200 OK sent to " << deviceId << " SN=" << sn;
    } else {
        InfoL << "【OTHER NOTIFY】From: " << deviceId;
    }

    // 发送 200 OK 响应
    sendSimpleResponse(rdata, 200);
}

/**
 * @brief PJSIP 请求处理回调函数
 * @param rdata PJSIP 接收数据
 * @return PJ_TRUE 表示已处理，PJ_FALSE 表示未处理
 * @details PJSIP 模块入口点，分发处理各种 SIP 请求：
 *          - REGISTER：设备注册/注销
 *          - MESSAGE：心跳、目录查询
 *          - NOTIFY：目录主动上报
 *          符合 GB28181-2016 第6章 SIP 消息格式要求
 */
pj_bool_t onRxRequest(pjsip_rx_data* rdata) {
    if (!rdata || !rdata->msg_info.msg) return PJ_FALSE;

    pjsip_msg* msg = rdata->msg_info.msg;

    // 只处理请求
    if (msg->type != PJSIP_REQUEST_MSG) return PJ_FALSE;

    // 获取方法名
    pj_str_t methodName = msg->line.req.method.name;
    std::string methodStr(methodName.ptr, methodName.slen);
    
    // 打印所有收到的请求
    InfoL << "【ONRXREQUEST】Method: " << methodStr;

    {
        std::string peerGbId;
        gb::UpstreamSignalMatch um = resolveUpstreamMatchForRequest(rdata, msg, &peerGbId);
        std::string sigIp;
        int sigPort = 0;
        extractSignalSrcFromRdata(rdata, sigIp, sigPort);
        if (um.matched && !um.enabled) {
            WarnL << "【ONRXREQUEST】403 拒绝: Method=" << methodStr
                  << " src=" << sigIp << ":" << sigPort
                  << " peerGbId=" << (peerGbId.empty() ? "(empty)" : peerGbId)
                  << " platformDbId=" << um.platformDbId;
            sendSimpleResponse(rdata, 403);
            return PJ_TRUE;
        }
    }

    if (methodName.slen == 7 && strncasecmp(methodName.ptr, "OPTIONS", 7) == 0) {
        pjsip_tx_data* tdata = nullptr;
        pj_status_t st = pjsip_endpt_create_response(g_sip_endpt, rdata, 200, nullptr, &tdata);
        if (st == PJ_SUCCESS && tdata) {
            pj_str_t hname = pj_str((char*)"Allow");
            pj_str_t hval = pj_str((char*)"INVITE, ACK, CANCEL, BYE, REGISTER, OPTIONS, MESSAGE, NOTIFY, INFO");
            pjsip_hdr* h = (pjsip_hdr*)pjsip_generic_string_hdr_create(tdata->pool, &hname, &hval);
            if (h) pjsip_msg_add_hdr(tdata->msg, h);
            pjsip_endpt_send_response2(g_sip_endpt, rdata, tdata, nullptr, nullptr);
        } else {
            sendSimpleResponse(rdata, 200);
        }
        return PJ_TRUE;
    }

    if (msg->line.req.method.id == PJSIP_INVITE_METHOD) {
        std::string sigIpInv;
        int sigPortInv = 0;
        extractSignalSrcFromRdata(rdata, sigIpInv, sigPortInv);
        gb::UpstreamSignalMatch umInv = gb::matchUpstreamBySignalSource(sigIpInv.c_str(), sigPortInv);
        InfoL << "【INVITE】src=" << sigIpInv << ":" << sigPortInv
              << " matched=" << umInv.matched << " enabled=" << umInv.enabled
              << " platformDbId=" << umInv.platformDbId;
        if (umInv.matched && umInv.enabled) {
            if (gb::tryHandleUpstreamPlatformInvite(rdata, umInv)) return PJ_TRUE;
        }
        InfoL << "【INVITE】非上级点播或未接管，返回 501";
        sendSimpleResponse(rdata, 501);
        return PJ_TRUE;
    }

    // 处理 REGISTER
    if (msg->line.req.method.id == PJSIP_REGISTER_METHOD) {
        InfoL << "【HANDLING REGISTER】";
        handleRegisterRequest(rdata);
        return PJ_TRUE;
    }

    // 处理 MESSAGE
    if (methodName.slen == 7 && strncasecmp(methodName.ptr, "MESSAGE", 7) == 0) {
        InfoL << "【HANDLING MESSAGE】";
        handleMessageRequest(rdata);
        return PJ_TRUE;
    }

    // 处理 NOTIFY (GB28181 Catalog 上报)
    if (methodName.slen == 6 && strncasecmp(methodName.ptr, "NOTIFY", 6) == 0) {
        InfoL << "【HANDLING NOTIFY】";
        handleNotifyRequest(rdata);
        return PJ_TRUE;
    }

    if (msg->line.req.method.id == PJSIP_CANCEL_METHOD) {
        std::string sigIpCa;
        int sigPortCa = 0;
        extractSignalSrcFromRdata(rdata, sigIpCa, sigPortCa);
        gb::UpstreamSignalMatch umCa = gb::matchUpstreamBySignalSource(sigIpCa.c_str(), sigPortCa);
        if (umCa.matched && umCa.enabled && gb::tryHandleUpstreamCancel(rdata, umCa)) return PJ_TRUE;
        sendSimpleResponse(rdata, 501);
        return PJ_TRUE;
    }

    // BYE：上级挂断点播桥接，或下级主动挂断
    if (msg->line.req.method.id == PJSIP_BYE_METHOD) {
        std::string sigIpBye;
        int sigPortBye = 0;
        extractSignalSrcFromRdata(rdata, sigIpBye, sigPortBye);
        gb::UpstreamSignalMatch umBye = gb::matchUpstreamBySignalSource(sigIpBye.c_str(), sigPortBye);
        if (umBye.matched && umBye.enabled && gb::tryHandleUpstreamBye(rdata, umBye)) return PJ_TRUE;

        std::string callIdStr;
        pjsip_cid_hdr* cid = (pjsip_cid_hdr*)pjsip_msg_find_hdr(msg, PJSIP_H_CALL_ID, nullptr);
        if (cid && cid->id.slen > 0) {
            callIdStr.assign(cid->id.ptr, cid->id.slen);
        }
        std::string srcIp;
        int srcPort = 0;
        extractSignalSrcFromRdata(rdata, srcIp, srcPort);
        InfoL << "【收到下级 BYE】Call-ID=" << callIdStr << " from=" << srcIp << ":" << srcPort
              << "（对端主动结束会话；若与抓包一致则非本端先发 BYE）";
        sendSimpleResponse(rdata, 200);
        return PJ_TRUE;
    }

    // 上级/对端对 INVITE 最终响应(如 486/200)发送的 ACK：无状态响应，吞掉即可
    if (msg->line.req.method.id == PJSIP_ACK_METHOD) {
        InfoL << "【ACK】收到（不回复 SIP 响应）";
        return PJ_TRUE;
    }

    InfoL << "【UNKNOWN METHOD】" << methodStr << " - returning 501";
    sendSimpleResponse(rdata, 501);
    return PJ_TRUE;
}

/**
 * @brief 为 INVITE 200 OK 发送 ACK 请求
 * @param rdata 收到的 200 OK 响应数据
 * @param from_hdr From 头
 * @param to_hdr To 头（包含 200 OK 的 To tag）
 * @param call_id_hdr Call-ID 头
 * @param cseq_hdr CSeq 头
 * @details SIP 三次握手：INVITE -> 200 OK -> ACK
 *          ACK 的 From/To/Call-ID 与 INVITE 相同
 *          CSeq 数字相同但方法改为 ACK
 * @note Request-URI 仍按 RFC3261 取自 200 OK 的 Contact；UDP 层目的地址强制为 200 OK 来源
 *       （NAT 下 Contact 多为内网，与 sendPlayInvite 的 signal_src 预填一致）
 */
void sendAckForInvite(pjsip_rx_data* rdata,
                       pjsip_from_hdr* from_hdr,
                       pjsip_to_hdr* to_hdr,
                       pjsip_cid_hdr* call_id_hdr,
                       pjsip_cseq_hdr* cseq_hdr) {
    if (!g_sip_endpt) return;

    // 创建内存池
    pj_pool_t* pool = pjsip_endpt_create_pool(g_sip_endpt, "ack_play", 2048, 1024);
    if (!pool) {
        ErrorL << "【sendAckForInvite】Failed to create pool";
        return;
    }

    // 克隆 From 头（与 INVITE 相同）
    pjsip_from_hdr* ack_from = (pjsip_from_hdr*)pjsip_hdr_clone(pool, (pjsip_hdr*)from_hdr);

    // 克隆 To 头（包含 200 OK 的 To tag）
    pjsip_to_hdr* ack_to = (pjsip_to_hdr*)pjsip_hdr_clone(pool, (pjsip_hdr*)to_hdr);

    // 克隆 Call-ID 头
    pjsip_cid_hdr* ack_call_id = (pjsip_cid_hdr*)pjsip_hdr_clone(pool, (pjsip_hdr*)call_id_hdr);

    // 获取目标 URI（从 200 OK 的 Contact 头构建，这是关键！）
    // 根据 RFC 3261，ACK 必须使用 200 OK 响应中的 Contact 地址
    std::string targetUriStr;
    pjsip_contact_hdr* contact_hdr = (pjsip_contact_hdr*)pjsip_msg_find_hdr(rdata->msg_info.msg, PJSIP_H_CONTACT, nullptr);
    if (contact_hdr && contact_hdr->uri) {
        char uri_buf[256];
        int uri_len = pjsip_uri_print(PJSIP_URI_IN_REQ_URI, contact_hdr->uri, uri_buf, sizeof(uri_buf));
        if (uri_len > 0) {
            targetUriStr = std::string(uri_buf, uri_len);
            InfoL << "【sendAckForInvite】Using Contact URI: " << targetUriStr;
        }
    }

    // 如果没有 Contact 头，使用 To 头作为备选
    if (targetUriStr.empty()) {
        char uri_buf[256];
        int uri_len = pjsip_uri_print(PJSIP_URI_IN_REQ_URI, ack_to->uri, uri_buf, sizeof(uri_buf));
        if (uri_len > 0) {
            targetUriStr = std::string(uri_buf, uri_len);
        } else {
            targetUriStr = "sip:unknown@192.168.1.133:5060";
        }
        InfoL << "【sendAckForInvite】No Contact header, using To URI: " << targetUriStr;
    }

    // 解析目标 URI
    pj_str_t targetUriPj = pj_str((char*)targetUriStr.c_str());
    pjsip_uri* target_uri = pjsip_parse_uri(pool, targetUriPj.ptr, targetUriPj.slen, PJSIP_PARSE_URI_AS_NAMEADDR);
    if (!target_uri) {
        ErrorL << "【sendAckForInvite】Failed to parse target URI: " << targetUriStr;
        pjsip_endpt_release_pool(g_sip_endpt, pool);
        return;
    }

    // 创建 ACK 方法
    pjsip_method ack_method;
    pjsip_method_set(&ack_method, PJSIP_ACK_METHOD);

    // 创建 ACK 请求
    pjsip_tx_data* tdata = nullptr;
    pj_status_t st = pjsip_endpt_create_request_from_hdr(g_sip_endpt,
                                                          &ack_method,
                                                          target_uri,
                                                          ack_from,
                                                          ack_to,
                                                          nullptr,  // Contact
                                                          ack_call_id,  // Call-ID
                                                          cseq_hdr->cseq,  // CSeq number
                                                          nullptr,  // body
                                                          &tdata);
    if (st != PJ_SUCCESS || !tdata) {
        ErrorL << "【sendAckForInvite】Failed to create ACK request: " << st;
        pjsip_endpt_release_pool(g_sip_endpt, pool);
        return;
    }

    // 修正 CSeq 方法为 ACK（create_request_from_hdr 可能不会正确设置）
    pjsip_cseq_hdr* ack_cseq = (pjsip_cseq_hdr*)pjsip_msg_find_hdr(tdata->msg, PJSIP_H_CSEQ, nullptr);
    if (ack_cseq) {
        pj_str_t ack_str = pj_str((char*)"ACK");
        ack_cseq->method.name = ack_str;
    }

    presetAckUdpDestFromInviteResponse(rdata, tdata);

    // 注意：pjsip_endpt_create_request_from_hdr 已经自动创建了 Via 头
    // 不需要手动添加，否则会导致重复的 Via 头

    // 打印 ACK 消息
    char msg_buf[2048];
    pj_ssize_t msg_len = pjsip_msg_print(tdata->msg, msg_buf, sizeof(msg_buf));
    if (msg_len > 0) {
        InfoL << "【sendAckForInvite】SIP ACK消息:\n" << std::string(msg_buf, msg_len);
    }

    // 发送 ACK（stateless）
    st = pjsip_endpt_send_request_stateless(g_sip_endpt, tdata, nullptr, nullptr);
    if (st != PJ_SUCCESS) {
        ErrorL << "【sendAckForInvite】Failed to send ACK: " << st;
        pjsip_endpt_release_pool(g_sip_endpt, pool);
        return;
    }

    InfoL << "【sendAckForInvite】ACK sent successfully for call " << std::string(ack_call_id->id.ptr, ack_call_id->id.slen);
    pjsip_endpt_release_pool(g_sip_endpt, pool);
}

/**
 * @brief PJSIP 响应处理回调函数
 * @param rdata PJSIP 接收数据（响应消息）
 * @return PJ_TRUE 表示已处理，PJ_FALSE 表示未处理
 * @details 处理 GB28181 协议中的 SIP 响应消息：
 *          - INVITE 200 OK：发送 ACK 完成三次握手
 *          - MESSAGE 200 OK：Catalog 查询响应、其他响应
 *          - 提取响应 body 中的业务数据（如 Catalog、DeviceInfo 等）
 */
pj_bool_t onRxResponse(pjsip_rx_data* rdata) {
    if (!rdata || !rdata->msg_info.msg) return PJ_FALSE;

    pjsip_msg* msg = rdata->msg_info.msg;

    // 只处理响应
    if (msg->type != PJSIP_RESPONSE_MSG) return PJ_FALSE;

    int statusCode = msg->line.status.code;
    pjsip_cseq_hdr* cseq = (pjsip_cseq_hdr*)pjsip_msg_find_hdr(msg, PJSIP_H_CSEQ, nullptr);
    
    if (!cseq) return PJ_FALSE;
    
    // 获取方法名
    std::string methodStr(cseq->method.name.ptr, cseq->method.name.slen);
    
    InfoL << "【ONRXRESPONSE】Status: " << statusCode << " Method: " << methodStr;

    // INVITE 最终响应：上级点播桥接（若有）+ 向下级发 ACK
    if (cseq->method.name.slen == 6 && strncasecmp(cseq->method.name.ptr, "INVITE", 6) == 0) {
        if (statusCode == 200) {
            InfoL << "【INVITE 200 OK】Received 200 OK for INVITE";
            gb::upstreamBridgeOnDeviceInviteRxResponse(rdata, 200);

            pjsip_from_hdr* from_hdr = (pjsip_from_hdr*)pjsip_msg_find_hdr(msg, PJSIP_H_FROM, nullptr);
            pjsip_to_hdr* to_hdr = (pjsip_to_hdr*)pjsip_msg_find_hdr(msg, PJSIP_H_TO, nullptr);
            pjsip_cid_hdr* call_id_hdr = (pjsip_cid_hdr*)pjsip_msg_find_hdr(msg, PJSIP_H_CALL_ID, nullptr);

            if (from_hdr && to_hdr && call_id_hdr) {
                sendAckForInvite(rdata, from_hdr, to_hdr, call_id_hdr, cseq);
            } else {
                WarnL << "【INVITE 200 OK】Missing required headers for ACK";
            }

            return PJ_TRUE;
        }
        if (statusCode >= 300) {
            gb::upstreamBridgeOnDeviceInviteRxResponse(rdata, statusCode);
            return PJ_TRUE;
        }
    }

    // 只处理 MESSAGE 方法的 200 OK 响应
    if (statusCode == 200 && cseq->method.name.slen == 7 && 
        strncasecmp(cseq->method.name.ptr, "MESSAGE", 7) == 0) {
        
        // 调试：打印消息信息
        InfoL << "【RESPONSE DEBUG】msg->body=" << (msg->body ? "yes" : "null") 
              << " len=" << (msg->body ? msg->body->len : 0);
        InfoL << "【RESPONSE DEBUG】pkt_info.len=" << rdata->pkt_info.len 
              << " pkt_info.packet=" << (rdata->pkt_info.packet ? "yes" : "null");
        
        // 提取 body - 尝试从 msg->body 获取
        std::string body;
        if (msg->body && msg->body->data && msg->body->len > 0) {
            body = std::string((char*)msg->body->data, msg->body->len);
        }
        
        // 如果 body 为空，尝试从 rdata 的 packet_info 获取原始数据
        if (body.empty() && rdata->pkt_info.packet && rdata->pkt_info.len > 0) {
            // 从原始数据包中解析 body（HTTP/SIP 消息体在空行之后）
            const char* raw_data = rdata->pkt_info.packet;
            int raw_len = rdata->pkt_info.len;
            
            // 调试：打印原始数据预览
            std::string raw_preview(raw_data, std::min(raw_len, 300));
            InfoL << "【RAW PACKET PREVIEW】" << raw_preview;
            
            // 查找空行分隔符（\r\n\r\n 或 \n\n）
            const char* body_start = nullptr;
            
            // 使用 std::string 查找（跨平台兼容）
            std::string raw_str(raw_data, raw_len);
            size_t pos_crlf = raw_str.find("\r\n\r\n");
            size_t pos_lf = raw_str.find("\n\n");
            
            InfoL << "【RAW PACKET DELIMITER】crlf=" << pos_crlf << " lf=" << pos_lf;
            
            if (pos_crlf != std::string::npos) {
                body_start = raw_data + pos_crlf + 4;
                InfoL << "【RAW PACKET】Found \\r\\n\\r\\n at " << pos_crlf;
            } else if (pos_lf != std::string::npos) {
                body_start = raw_data + pos_lf + 2;
                InfoL << "【RAW PACKET】Found \\n\\n at " << pos_lf;
            }
            
            if (body_start && body_start < raw_data + raw_len) {
                size_t body_len = raw_data + raw_len - body_start;
                body = std::string(body_start, body_len);
                InfoL << "【RESPONSE BODY FROM RAW】Size: " << body_len;
            } else {
                InfoL << "【RAW PACKET】No body found";
            }
        }
        
        if (!body.empty()) {
            InfoL << "【MESSAGE RESPONSE BODY】Size: " << body.size();
            InfoL << "【BODY PREVIEW】" << body.substr(0, 400);
            
            // 检查是否是 Catalog 响应
            bool hasCatalog = (body.find("<CmdType>Catalog</CmdType>") != std::string::npos);
            bool hasResponse = (body.find("<Response>") != std::string::npos);
            bool hasNotify = (body.find("<CmdType>Notify</CmdType>") != std::string::npos);
            
            InfoL << "【RESPONSE CMDTYPE】Catalog=" << hasCatalog 
                  << " Response=" << hasResponse << " Notify=" << hasNotify;
            
            // 提取设备ID（从 Call-ID 或 From）
            std::string deviceId;
            pjsip_from_hdr* from = (pjsip_from_hdr*)pjsip_msg_find_hdr(msg, PJSIP_H_FROM, nullptr);
            if (from && from->uri) {
                deviceId = parseDeviceIdFromUri(from->uri);
            }
            
            if (hasCatalog || hasNotify) {
                InfoL << "【CATALOG RESPONSE DETECTED】From: " << deviceId;
                
                // 调用 Catalog 处理函数
                handleCatalogNotify(deviceId, body);
                
                InfoL << "【CATALOG RESPONSE】Processed from " << deviceId;
                return PJ_TRUE;
            }
        } else {
            InfoL << "【MESSAGE RESPONSE】Empty body, skipping";
        }
    }

    return PJ_TRUE;
}

// PJSIP 模块定义
static pjsip_module g_sip_module = {
    nullptr, nullptr,             // prev, next
    { (char*)"mod-gb28181", 10 }, // name
    -1,                           // id
    PJSIP_MOD_PRIORITY_APPLICATION, // priority
    nullptr,                      // load
    nullptr,                      // start
    nullptr,                      // stop
    nullptr,                      // unload
    &onRxRequest,                 // on_rx_request
    &onRxResponse,                // on_rx_response - 处理响应消息
    nullptr,                      // on_tx_request
    nullptr,                      // on_tx_response
};

/**
 * @brief PJSIP 工作线程函数
 * @details 后台线程主循环：
 *          1. 注册为 PJSIP 线程
 *          2. 处理 SIP 事件（100ms 超时）
 *          3. 处理待发送的 Catalog 查询队列
 *          确保 PJSIP 事件及时处理和异步 Catalog 查询发送
 */
void workerThread() {
    // 将当前 std::thread 注册为 pjlib 线程
    pj_thread_desc desc;
    pj_thread_t* this_thread = nullptr;
    pj_status_t st = pj_thread_register("pjsip_worker", desc, &this_thread);
    if (st != PJ_SUCCESS) {
        ErrorL << "pj_thread_register failed in workerThread: " << st;
        return;
    }

    InfoL << "PJSIP worker thread started";

    while (g_running) {
        // 处理 PJSIP 事件
        pj_time_val timeout = { 0, 100 }; // 100ms
        pjsip_endpt_handle_events(g_sip_endpt, &timeout);

        // 处理待发送的 Catalog 查询
        processPendingCatalogQueries();

        gb::processPendingRecordInfoQueries();

        // 处理待发送的 INVITE 请求（视频预览）
        gb::processPendingInvites();

        gb::processUpstreamZlmSourceWaitPoll();

        gb::upstreamRegistrarProcessMaintenance();
    }

    InfoL << "PJSIP worker thread stopped";
}

} // namespace

/**
 * @brief 加载 SIP 服务配置
 * @return 解析后的 SIP 服务配置
 * @details 从 gb_local_config 表查询：
 *          - signal_port: 信令端口
 *          - transport_udp: 是否启用 UDP
 *          - transport_tcp: 是否启用 TCP
 *          若查询失败使用默认值（5060, UDP）
 */
SipServerConfig loadSipServerConfig() {
    SipServerConfig cfg;

    std::string sql = "SELECT signal_port, transport_udp, transport_tcp FROM gb_local_config LIMIT 1";
    std::string out = execPsql(sql.c_str());

    if (!out.empty()) {
        std::vector<std::string> cols = split(out, "|");
        if (cols.size() >= 3) {
            cfg.signal_port = std::atoi(trim(std::string(cols[0])).c_str());
            std::string udpFlag = trim(std::string(cols[1]));
            std::string tcpFlag = trim(std::string(cols[2]));
            cfg.transport_udp = (udpFlag == "t" || udpFlag == "true" || udpFlag == "1");
            cfg.transport_tcp = (tcpFlag == "t" || tcpFlag == "true" || tcpFlag == "1");
        }
    }

    if (cfg.signal_port == 0) cfg.signal_port = 5060;

    return cfg;
}

/**
 * @brief 启动 PJSIP SIP 服务
 * @param cfg SIP 服务配置
 * @return true 启动成功，false 启动失败
 * @details 启动流程：
 *          1. 初始化 PJLIB 库
 *          2. 初始化缓存内存池
 *          3. 创建 PJSIP 端点
 *          4. 根据配置启动 UDP/TCP 传输
 *          5. 注册 SIP 模块
 *          6. 启动工作线程
 * @note 若启动失败会自动清理已分配资源
 */
bool SipServerPjsipStart(const SipServerConfig& cfg) {
    if (g_sip_endpt) return false;

    g_config = cfg;
    
    // 预加载系统配置到内存缓存（高性能，避免运行时频繁读数据库）
    g_systemConfig.loadFromDb();
    InfoL << "【SipServerPjsipStart】System config preloaded: realm=" << g_systemConfig.realm;

    // 初始化 PJLIB
    pj_status_t st = pj_init();
    if (st != PJ_SUCCESS) {
        ErrorL << "pj_init failed: " << st;
        pj_shutdown();
        return false;
    }

    // 初始化缓存内存池（只初始化一次）
    if (!g_cp_inited) {
        pj_caching_pool_init(&g_cp, &pj_pool_factory_default_policy, 0);
        g_cp_inited = true;
    }

    // 创建端点，使用缓存内存池的 factory
    st = pjsip_endpt_create(&g_cp.factory, "gb_service", &g_sip_endpt);
    if (st != PJ_SUCCESS) {
        ErrorL << "pjsip_endpt_create failed: " << st;
        pj_shutdown();
        return false;
    }

    st = pjsip_tsx_layer_init_module(g_sip_endpt);
    if (st != PJ_SUCCESS) {
        ErrorL << "pjsip_tsx_layer_init_module failed: " << st;
        pjsip_endpt_destroy(g_sip_endpt);
        g_sip_endpt = nullptr;
        pj_shutdown();
        return false;
    }

    /**
     * @note GB28181兼容性配置
     * @details 启用 From/To 头中的端口显示
     *          PJSIP默认遵循RFC 3261 Section 19.1.1，不在From/To头中显示端口
     *          但某些GB28181厂商平台（如海康、大华）需要完整host:port格式
     *          来正确识别发送方和路由响应消息
     *          正确格式：sip:device@host:port
     *          错误格式：sip:device@host（缺少端口）
     */
    pjsip_cfg()->endpt.allow_port_in_fromto_hdr = PJ_TRUE;
    InfoL << "【SipServerPjsipStart】GB28181模式：已启用 From/To 头端口显示";

    // 添加 UDP 传输
    if (cfg.transport_udp) {
        pj_sockaddr_in addr;
        pj_sockaddr_in_init(&addr, nullptr, cfg.signal_port);
        st = pjsip_udp_transport_start(g_sip_endpt, &addr, nullptr, 1, nullptr);
        if (st != PJ_SUCCESS) {
            ErrorL << "UDP transport start failed: " << st;
        } else {
            InfoL << "SIP UDP transport started on port " << cfg.signal_port;
        }
    }

    // 添加 TCP 传输（与 UDP 同端口；published 地址传 NULL，由 PJSIP 按绑定套接字推导，避免 0.0.0.0 触发 PJ_EINVAL）
    if (cfg.transport_tcp) {
        pj_sockaddr_in tcpAddr;
        pj_sockaddr_in_init(&tcpAddr, nullptr, cfg.signal_port);
        pjsip_tpfactory* tcpFactory = nullptr;
        st = pjsip_tcp_transport_start(g_sip_endpt, &tcpAddr, 1, &tcpFactory);
        if (st != PJ_SUCCESS) {
            ErrorL << "TCP transport start failed: " << st;
        } else {
            InfoL << "SIP TCP transport started on port " << cfg.signal_port;
        }
    }

    // 注册模块
    st = pjsip_endpt_register_module(g_sip_endpt, &g_sip_module);
    if (st != PJ_SUCCESS) {
        ErrorL << "Module register failed: " << st;
        return false;
    }

    // 启动工作线程
    g_running = true;
    g_workerThread = std::thread(workerThread);

    gb::requestUpstreamRegistrarReload();

    InfoL << "PJSIP SIP server started";
    return true;
}

/**
 * @brief 停止 PJSIP SIP 服务
 * @details 关闭流程：
 *          1. 设置停止标志，通知工作线程退出
 *          2. 等待工作线程结束
 *          3. 销毁 PJSIP 端点
 *          4. 销毁缓存内存池
 *          5. 关闭 PJLIB
 * @note 优雅关闭，确保资源正确释放
 */
void SipServerPjsipStop() {
    g_running = false;

    if (g_workerThread.joinable()) {
        g_workerThread.join();
    }

    if (g_sip_endpt) {
        pjsip_endpt_destroy(g_sip_endpt);
        g_sip_endpt = nullptr;
    }

    // 销毁缓存内存池
    if (g_cp_inited) {
        pj_caching_pool_destroy(&g_cp);
        g_cp_inited = false;
    }

    pj_shutdown();
    InfoL << "PJSIP SIP server stopped";
}

/**
 * @brief 刷新系统配置缓存
 * @details 重新从数据库加载配置到内存缓存，供 HTTP 配置更新接口调用
 * @note 线程安全，可在运行时调用
 */
void reloadSystemConfigCache() {
    g_systemConfig.reload();
}

/**
 * @brief 确保系统配置已加载（辅助函数）
 */
static void ensureSystemConfigLoaded() {
    if (!g_systemConfig.loaded) {
        g_systemConfig.loadFromDb();
    }
}

/**
 * @brief 获取系统配置缓存的 realm（SIP 域）
 */
const char* getSystemRealm() {
    ensureSystemConfigLoaded();
    return g_systemConfig.realm.c_str();
}

/**
 * @brief 获取系统配置缓存的密码
 */
const char* getSystemPassword() {
    ensureSystemConfigLoaded();
    return g_systemConfig.password.c_str();
}

/**
 * @brief 获取系统配置缓存的本机国标 ID
 */
const char* getSystemGbId() {
    ensureSystemConfigLoaded();
    return g_systemConfig.localGbId.c_str();
}

bool isCatalogOnRegisterEnabled() {
    ensureSystemConfigLoaded();
    return g_systemConfig.catalogOnRegisterEnabled;
}

int getCatalogOnRegisterCooldownSec() {
    ensureSystemConfigLoaded();
    return g_systemConfig.catalogOnRegisterCooldownSec;
}

} // namespace gb
