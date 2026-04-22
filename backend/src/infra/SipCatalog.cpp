/**
 * @file SipCatalog.cpp
 * @brief GB28181 SIP 目录查询、点播 INVITE/BYE、云台 DeviceControl 等业务实现
 * @details 协议关联：GB/T 28181-2016 目录查询（MESSAGE）、实时预览（INVITE/SDP）、
 *          云台控制（MESSAGE + CmdType DeviceControl + PTZCmd）。
 *          信令出口与 NAT：与 Catalog 共用 loadPlatformSipRouting + presetTxDataUdpDestFromSignal。
 * @dependencies PJSIP、DbUtil(execPsql)、SipServerPjsip(getSystemGbId)、本模块内异步队列
 * @date 2025
 * @note INVITE/BYE/PTZ 异步项均在 processPendingInvites() 中由 PJSIP 工作线程消费；勿在 HTTP 线程直接 send。
 */
#include "infra/SipCatalog.h"
#include "infra/DbUtil.h"
#include "infra/AuthHelper.h"
#include "infra/SipServerPjsip.h"  // 用于获取预读缓存的系统配置
#include "infra/UpstreamInviteBridge.h"
#include "Util/logger.h"
#include <mutex>
#include <vector>
#include <pjsip.h>
#include <pjlib.h>
#include <cstring>
#include <sstream>
#include <map>
#include <iconv.h>  // GB2312 转 UTF-8 编码转换
#include <random>   // for std::random_device, std::mt19937
#include <algorithm>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <atomic>
#include <ctime>
#include <memory>
#include <unordered_set>

using namespace toolkit;

namespace gb {

// 外部 PJSIP 端点引用（由 SipServerPjsip 初始化）
extern pjsip_endpoint* g_sip_endpt;

namespace {

static std::string trimSqlField(std::string s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

/**
 * @brief 解析 loadPlatformSipRouting 查询的首行（| 分隔四列）
 */
static bool parsePlatformSipRow(const std::string& out,
                                std::string& contactHost, int& contactPort,
                                std::string& signalHost, int& signalPort) {
  size_t nlPos = out.find('\n');
  std::string line = (nlPos == std::string::npos) ? out : out.substr(0, nlPos);
  std::vector<std::string> cols;
  std::string cur;
  for (char c : line) {
    if (c == '|') {
      cols.push_back(trimSqlField(cur));
      cur.clear();
    } else
      cur += c;
  }
  cols.push_back(trimSqlField(cur));
  if (cols.size() < 2) return false;
  contactHost = cols[0];
  contactPort = cols[1].empty() ? 5060 : std::atoi(cols[1].c_str());
  signalHost = cols.size() > 2 ? cols[2] : "";
  signalPort = 0;
  if (cols.size() > 3 && !cols[3].empty()) signalPort = std::atoi(cols[3].c_str());
  return true;
}

/**
 * @brief 从 device_platforms 读取 Contact 与 NAT 出口 signal_src（Catalog / INVITE / BYE 共用）
 */
static bool loadPlatformSipRouting(const std::string& platformGbId,
                                   std::string& contactHost, int& contactPort,
                                   std::string& signalHost, int& signalPort) {
  std::string sql =
      "SELECT COALESCE(NULLIF(TRIM(contact_ip), ''), '127.0.0.1'), "
      "COALESCE(NULLIF(contact_port, 0), 5060), "
      "NULLIF(TRIM(signal_src_ip), ''), "
      "COALESCE(NULLIF(signal_src_port, 0), 0) "
      "FROM device_platforms "
      "WHERE gb_id = '" + gb::escapeSqlString(platformGbId) + "'";
  std::string out = execPsql(sql.c_str());
  if (out.empty()) return false;
  return parsePlatformSipRow(out, contactHost, contactPort, signalHost, signalPort);
}

/**
 * @brief 预填 tdata->dest_info，使 stateless 发往 signal_src（R-URI 仍可保持 Contact）
 */
static void presetTxDataUdpDestFromSignal(pjsip_tx_data* tdata,
                                          const std::string& signalHost, int signalPort,
                                          const char* logTag,
                                          const std::string& contactLabel) {
  tdata->dest_info.cur_addr = 0;
  if (signalHost.empty() || signalPort <= 0) {
    tdata->dest_info.addr.count = 0;
    return;
  }
  pjsip_server_address_record* rec = &tdata->dest_info.addr.entry[0];
  pj_bzero(rec, sizeof(*rec));
  pj_str_t sh = pj_str((char*)signalHost.c_str());
  pj_status_t pst;
#if defined(PJ_HAS_IPV6) && PJ_HAS_IPV6
  if (strchr(signalHost.c_str(), ':') != nullptr)
    pst = pj_sockaddr_init(pj_AF_INET6(), &rec->addr, &sh, (pj_uint16_t)signalPort);
  else
#endif
    pst = pj_sockaddr_init(pj_AF_INET(), &rec->addr, &sh, (pj_uint16_t)signalPort);
  if (pst == PJ_SUCCESS) {
    rec->addr_len = pj_sockaddr_get_len(&rec->addr);
    rec->type = PJSIP_TRANSPORT_UDP;
    rec->priority = 0;
    rec->weight = 0;
    tdata->dest_info.addr.count = 1;
    InfoL << logTag << " 预置 UDP 目的 signal_src=" << signalHost << ":" << signalPort
          << "（SIP 语义仍为 Contact " << contactLabel << "）";
  } else {
    WarnL << logTag << " signal_src 解析失败，按 R-URI 解析目的: " << signalHost;
    tdata->dest_info.addr.count = 0;
  }
}

/** Contact 是否可作为 SIP URI host（与 Catalog 一致） */
static bool contactUsableAsUriHost(const std::string& host) {
  return !host.empty() && host != "127.0.0.1";
}

/**
 * @brief 决定 INVITE/BYE 等使用的 URI host:port；Contact 不可用时退化为 signal_src
 */
static bool resolveUriHostForDownlink(const std::string& platformGbId,
                                      const std::string& contactHost, int contactPort,
                                      const std::string& signalHost, int signalPort,
                                      std::string& uriHost, int& uriPort,
                                      const char* logTag) {
  if (contactUsableAsUriHost(contactHost)) {
    uriHost = contactHost;
    uriPort = contactPort > 0 ? contactPort : 5060;
    return true;
  }
  if (!signalHost.empty() && signalPort > 0) {
    uriHost = signalHost;
    uriPort = signalPort;
    WarnL << logTag << " 平台 " << platformGbId
          << " Contact 无效，URI host 退化为 signal_src " << uriHost << ":" << uriPort;
    return true;
  }
  return false;
}

/**
 * @brief 判断 action 字符串是否与字面量相同（忽略大小写）
 * @param a HTTP/JSON 传入的 action
 * @param lit 小写字面量，如 "start" / "stop"
 * @return 相同为 true
 */
static bool ptzActionEquals(const std::string& a, const char* lit) {
  if (a.size() != std::strlen(lit)) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(lit[i])))
      return false;
  }
  return true;
}

/**
 * @brief 生成 GB28181 DeviceControl 所需 PTZCmd（16 位大写十六进制，表示 8 字节）
 * @param command up/down/left/right/zoomIn/zoomOut/irisOpen/irisClose/stop（大小写不敏感，camelCase 会规范化）
 * @param action start（开始动作）或 stop（停止）；与 command=stop 组合表示全停
 * @param speed 速度档 1–3，映射为指令字中的 0x10/0x20/0x30
 * @return 合法则 16 字符 HEX；未知 command/action 返回空串
 * @details 附录 A.3 常用实现：固定头 A5 0F 01 08；字节4 为子命令；字节5/6 为水平/垂直速度；
 *          字节8 = (字节1..7 之和) & 0xFF。厂商差异时优先调整字节4 映射表。
 * @note 与 HttpSession::handlePtzControl 的 JSON 字段约定保持一致
 */
static std::string gb28181BuildPtzCmdHex(const std::string& command, const std::string& action, int speed) {
  unsigned char b[8];
  b[0] = 0xA5;
  b[1] = 0x0F;
  b[2] = 0x01;
  b[3] = 0x08;
  b[4] = 0;
  b[5] = 0;
  b[6] = 0;

  int sp = speed;
  if (sp < 1) sp = 1;
  if (sp > 3) sp = 3;
  unsigned char spd = static_cast<unsigned char>(sp * 0x10);

  std::string cmd = command;
  for (auto& ch : cmd) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

  if (ptzActionEquals(action, "stop") || cmd == "stop") {
    // 全停：子命令与速度清零
  } else if (ptzActionEquals(action, "start")) {
    if (cmd == "up") {
      b[4] = 0x08;
      b[6] = spd;
    } else if (cmd == "down") {
      b[4] = 0x04;
      b[6] = spd;
    } else if (cmd == "left") {
      b[4] = 0x02;
      b[5] = spd;
    } else if (cmd == "right") {
      b[4] = 0x01;
      b[5] = spd;
    } else if (cmd == "zoomin") {
      b[4] = 0x10;
      b[5] = spd;
    } else if (cmd == "zoomout") {
      b[4] = 0x20;
      b[5] = spd;
    } else if (cmd == "irisopen") {
      b[4] = 0x40;
      b[5] = spd;
    } else if (cmd == "irisclose") {
      b[4] = 0x80;
      b[5] = spd;
    } else {
      return "";
    }
  } else {
    return "";
  }

  unsigned sum = 0;
  for (int i = 0; i < 7; ++i) sum += b[i];
  b[7] = static_cast<unsigned char>(sum & 0xFF);

  static const char* hexd = "0123456789ABCDEF";
  std::string out;
  out.reserve(16);
  for (int i = 0; i < 8; ++i) {
    out.push_back(hexd[b[i] >> 4]);
    out.push_back(hexd[b[i] & 0x0F]);
  }
  return out;
}

/**
 * @brief 去除配置查询结果首尾空白与换行（供 PTZ MESSAGE 构造读 gb_local_config）
 * @param s psql 单列或分段字符串
 * @return 去空白后的子串；全空白返回空
 */
static std::string ptzTrimConfigField(const std::string& s) {
  if (s.empty()) return "";
  size_t start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return "";
  size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

/**
 * @brief 发送 SIP MESSAGE（Application/MANSCDP+xml）云台 DeviceControl
 * @param platformGbId 下级平台国标 ID（Request-URI user，与点播一致）
 * @param channelId 受控通道/摄像机国标 ID，写入 XML DeviceID
 * @param ptzCmdHex 8 字节 PTZ 指令的 16 字符大写十六进制
 * @return 成功发送 stateless MESSAGE 为 true
 * @details 信令路径与 sendCatalogQuery 一致：R-URI=sip:平台@Contact；tdata->dest_info 预填 signal_src。
 *          SN 取毫秒时间戳低位；Call-ID 前缀 ptz_。
 * @note 仅允许在持有 PJSIP 线程模型的上下文中调用（由 processPendingInvites 触发）
 */
static bool sendPtzDeviceControlMessage(const std::string& platformGbId,
                                        const std::string& channelId,
                                        const std::string& ptzCmdHex) {
  if (!g_sip_endpt) {
    ErrorL << "【PTZ】SIP endpoint not initialized";
    return false;
  }
  if (platformGbId.empty() || channelId.empty() || ptzCmdHex.size() != 16) {
    WarnL << "【PTZ】invalid args platform=" << platformGbId << " channel=" << channelId
          << " cmdLen=" << ptzCmdHex.size();
    return false;
  }

  std::string targetHost, signalHost;
  int targetPort = 5060, signalPort = 0;
  if (!loadPlatformSipRouting(platformGbId, targetHost, targetPort, signalHost, signalPort)) {
    ErrorL << "【PTZ】Platform not found: " << platformGbId;
    return false;
  }
  if (!contactUsableAsUriHost(targetHost)) {
    ErrorL << "【PTZ】Contact unavailable for platform " << platformGbId;
    return false;
  }

  std::string localGbId = getSystemGbId();
  if (localGbId.empty()) localGbId = "34020000002000000001";

  int localPort = 5060;
  std::string localHost = "127.0.0.1";
  std::string sql = "SELECT COALESCE(signal_ip, '127.0.0.1'), COALESCE(signal_port, 5060) FROM gb_local_config LIMIT 1";
  std::string out = execPsql(sql.c_str());
  if (!out.empty()) {
    size_t pipePos = out.find('|');
    if (pipePos != std::string::npos) {
      localHost = ptzTrimConfigField(out.substr(0, pipePos));
      std::string portStr = ptzTrimConfigField(out.substr(pipePos + 1));
      if (!portStr.empty()) localPort = std::atoi(portStr.c_str());
    } else {
      localHost = ptzTrimConfigField(out);
    }
  }
  if (localHost.empty()) localHost = "127.0.0.1";

  auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();
  int sn = static_cast<int>(nowMs & 0x7fffffff);

  std::string targetUri = "sip:" + platformGbId + "@" + targetHost + ":" + std::to_string(targetPort);
  std::string localUri = "sip:" + localGbId + "@" + localHost + ":" + std::to_string(localPort);

  std::string xmlBody = std::string("<?xml version=\"1.0\"?>\r\n") + "<Control>\r\n" + "<CmdType>DeviceControl</CmdType>\r\n" +
                        "<SN>" + std::to_string(sn) + "</SN>\r\n" + "<DeviceID>" + channelId + "</DeviceID>\r\n" + "<PTZCmd>" +
                        ptzCmdHex + "</PTZCmd>\r\n" + "</Control>";

  pj_pool_t* pool = pjsip_endpt_create_pool(g_sip_endpt, "ptz_dc", 2048, 1024);
  if (!pool) {
    ErrorL << "【PTZ】Failed to create pool";
    return false;
  }

  pjsip_uri* target_uri = pjsip_parse_uri(pool, (char*)targetUri.c_str(), (int)targetUri.length(), 0);
  if (!target_uri) {
    ErrorL << "【PTZ】Failed to parse target URI";
    pjsip_endpt_release_pool(g_sip_endpt, pool);
    return false;
  }

  pjsip_from_hdr* from_hdr = pjsip_from_hdr_create(pool);
  if (from_hdr) {
    pjsip_name_addr* from_name_addr = pjsip_name_addr_create(pool);
    if (from_name_addr) {
      pjsip_sip_uri* from_uri = pjsip_sip_uri_create(pool, PJ_FALSE);
      if (from_uri) {
        pj_strdup2(pool, &from_uri->user, (char*)localGbId.c_str());
        pj_strdup2(pool, &from_uri->host, (char*)localHost.c_str());
        from_uri->port = localPort;
        from_name_addr->uri = (pjsip_uri*)from_uri;
      }
      from_hdr->uri = (pjsip_uri*)from_name_addr;
    }
  }

  pjsip_to_hdr* to_hdr = pjsip_to_hdr_create(pool);
  if (to_hdr) {
    pjsip_name_addr* to_name_addr = pjsip_name_addr_create(pool);
    if (to_name_addr) {
      pjsip_sip_uri* to_uri = pjsip_sip_uri_create(pool, PJ_FALSE);
      if (to_uri) {
        pj_strdup2(pool, &to_uri->user, (char*)platformGbId.c_str());
        pj_strdup2(pool, &to_uri->host, (char*)targetHost.c_str());
        to_uri->port = targetPort;
        to_name_addr->uri = (pjsip_uri*)to_uri;
      }
      to_hdr->uri = (pjsip_uri*)to_name_addr;
    }
  }

  pj_str_t method_str = pj_str((char*)"MESSAGE");
  pjsip_method method;
  pjsip_method_init_np(&method, &method_str);

  static int ptz_cseq = 1;
  pjsip_tx_data* tdata = nullptr;
  pj_status_t st = pjsip_endpt_create_request_from_hdr(g_sip_endpt, &method, target_uri, from_hdr, to_hdr, nullptr, nullptr,
                                                       ptz_cseq++, nullptr, &tdata);
  if (st != PJ_SUCCESS || !tdata) {
    ErrorL << "【PTZ】Failed to create MESSAGE: " << st;
    pjsip_endpt_release_pool(g_sip_endpt, pool);
    return false;
  }

  presetTxDataUdpDestFromSignal(tdata, signalHost, signalPort, "【PTZ】",
                                targetHost + ":" + std::to_string(targetPort));

  pjsip_via_hdr* via = (pjsip_via_hdr*)pjsip_msg_find_hdr(tdata->msg, PJSIP_H_VIA, nullptr);
  if (via) {
    pj_strdup2(pool, &via->transport, (char*)"UDP");
    pj_strdup2(pool, &via->sent_by.host, (char*)localHost.c_str());
    via->sent_by.port = localPort;
    char branch_buf[64];
    snprintf(branch_buf, sizeof(branch_buf), "z9hG4bKptz%d", sn);
    pj_strdup2(pool, &via->branch_param, branch_buf);
  }

  char callIdBuf[80];
  snprintf(callIdBuf, sizeof(callIdBuf), "ptz_%d@%s", sn, localGbId.c_str());
  pjsip_cid_hdr* call_id_hdr = (pjsip_cid_hdr*)pjsip_msg_find_hdr(tdata->msg, PJSIP_H_CALL_ID, nullptr);
  if (call_id_hdr) pj_strdup2(pool, &call_id_hdr->id, callIdBuf);

  pj_str_t body_str = pj_str((char*)xmlBody.c_str());
  pj_str_t type_str = pj_str((char*)"Application");
  pj_str_t subtype_str = pj_str((char*)"MANSCDP+xml");
  tdata->msg->body = pjsip_msg_body_create(pool, &type_str, &subtype_str, &body_str);

  pjsip_ctype_hdr* existing_ct = (pjsip_ctype_hdr*)pjsip_msg_find_hdr(tdata->msg, PJSIP_H_CONTENT_TYPE, nullptr);
  if (existing_ct && existing_ct->next) pj_list_erase(existing_ct);

  char msg_buf[2048];
  pj_ssize_t msg_len = pjsip_msg_print(tdata->msg, msg_buf, sizeof(msg_buf));
  if (msg_len > 0) InfoL << "【PTZ DeviceControl】SIP MESSAGE:\n" << std::string(msg_buf, msg_len);

  st = pjsip_endpt_send_request_stateless(g_sip_endpt, tdata, nullptr, nullptr);
  if (st != PJ_SUCCESS) {
    ErrorL << "【PTZ】send stateless failed: " << st;
    pjsip_endpt_release_pool(g_sip_endpt, pool);
    return false;
  }

  InfoL << "【PTZ】DeviceControl sent platform=" << platformGbId << " channel=" << channelId << " PTZCmd=" << ptzCmdHex
        << " R-URI=" << targetUri;
  pjsip_endpt_release_pool(g_sip_endpt, pool);
  return true;
}

}  // namespace

// 异步查询队列
struct PendingCatalogQuery {
  std::string platformGbId;
  int sn;
};
std::vector<PendingCatalogQuery> g_pendingQueries;
std::mutex g_queriesMutex;

struct PendingRecordInfoQuery {
  std::string platformGbId;
  std::string channelId;
  std::string startGb;
  std::string endGb;
  int sn;
};
std::vector<PendingRecordInfoQuery> g_pendingRecordInfoQueries;
std::mutex g_recordInfoQueriesMutex;

struct RecordInfoWaitSlot {
  std::mutex mtx;
  std::condition_variable cv;
  bool done{false};
  bool success{false};
  std::string error;
  int64_t taskDbId{0};
  std::string cameraId;   /**< 设备国标 ID（日志 / SIP） */
  std::string cameraDbId; /**< cameras.id（BIGINT 字符串），写 replay_segments 去重 SQL */
};
std::map<int, std::shared_ptr<RecordInfoWaitSlot>> g_recordInfoWaitBySn;
std::mutex g_recordInfoWaitMutex;
std::atomic<int> g_recordInfoSnCounter{1};

// 异步INVITE队列
struct PendingInvite {
  std::string deviceGbId;
  std::string channelId;
  uint16_t zlmPort;
  std::string callId;
  std::string sdpConnectionIp;
  bool rtpOverTcp = false;
  uint64_t playbackStartUnix = 0;
  uint64_t playbackEndUnix = 0;
  bool isDownload = false; /**< true 时 SDP 使用 s=Download（设备以最快速率推流） */
};
std::vector<PendingInvite> g_pendingInvites;
std::mutex g_invitesMutex;
static int g_inviteCounter = 0;

// 异步BYE队列
struct PendingBye {
  std::string callId;
  std::string deviceGbId;
  std::string channelId;  // 摄像头ID，用于构建To头和Request URI
};
std::vector<PendingBye> g_pendingByes;
std::mutex g_byesMutex;

/**
 * @brief 待发送的回放倍速 INFO 任务
 * @details GB28181-2016 Annex A.2.5 / B.5：通过 SIP INFO + MANSRTSP 控制回放速率。
 *          INFO body 格式:
 *            PLAY MANSRTSP/1.0\r\n
 *            CSeq: <cseq>\r\n
 *            Scale: <speed>\r\n
 *            Range: npt=now-\r\n
 */
struct PendingPlaybackSpeed {
  std::string callId;       /**< 回放 INVITE 会话的 Call-ID */
  std::string deviceGbId;   /**< 下级平台/设备国标 ID */
  std::string channelId;    /**< 通道/摄像机国标 ID */
  double scale;             /**< 倍速值：0.25 / 0.5 / 1 / 2 / 4 / 8 / 16 / 32 等 */
};
std::vector<PendingPlaybackSpeed> g_pendingPlaybackSpeed;
std::mutex g_playbackSpeedMutex;
static std::atomic<int> g_infoCSeqCounter{1};

/**
 * @brief 待发送的云台 DeviceControl 任务（已由 HTTP 侧编码为 PTZCmd HEX）
 * @note 与 enqueuePtzDeviceControl / sendPtzDeviceControlMessage 配对
 */
struct PendingPtz {
  std::string platformGbId; /**< 下级平台国标 ID */
  std::string channelId;    /**< 通道/摄像机国标 ID */
  std::string ptzCmdHex;    /**< 16 字符 PTZCmd */
};
std::vector<PendingPtz> g_pendingPtz;
std::mutex g_ptzMutex;

namespace {
  
  // 回调函数
  CatalogResponseCallback g_catalogCallback = nullptr;
  
  // 正在进行的查询记录
  std::map<std::string, bool> g_queryInProgress;
  
  // 生成 XML 安全转义
  std::string xmlEscape(const std::string& str) {
    std::string result;
    for (char c : str) {
      switch (c) {
        case '&': result += "&amp;"; break;
        case '<': result += "&lt;"; break;
        case '>': result += "&gt;"; break;
        case '"': result += "&quot;"; break;
        case '\'': result += "&apos;"; break;
        default: result += c;
      }
    }
    return result;
  }
  
  // 从 XML 提取标签内容
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
  
  // 从 XML 提取 Int 类型的标签
  int extractXmlTagInt(const std::string& xml, const std::string& tagName, int defaultVal = 0) {
    std::string val = extractXmlTag(xml, tagName);
    if (val.empty()) return defaultVal;
    return std::atoi(val.c_str());
  }
  
  // 从 XML 标签属性中提取值（如 <Item Index="3"> 返回 "3"）
  std::string extractXmlAttribute(const std::string& xml, const std::string& attrName) {
    std::string attrPattern = attrName + "=\"";
    size_t start = xml.find(attrPattern);
    if (start == std::string::npos) return "";
    start += attrPattern.length();
    
    size_t end = xml.find("\"", start);
    if (end == std::string::npos) return "";
    
    return xml.substr(start, end - start);
  }
  
  // 清理字符串（去除首尾空白和换行）
  std::string trimString(const std::string& s) {
    if (s.empty()) return "";
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
  }
  
  /**
   * @brief GB2312 编码字符串转换为 UTF-8
   * @param gb2312Str GB2312 编码的字符串
   * @return UTF-8 编码的字符串，转换失败返回原字符串
   * @note 下级平台发送的 XML 使用 GB2312 编码，需要转换为 UTF-8 才能正确显示中文
   */
  std::string gb2312ToUtf8(const std::string& gb2312Str) {
    if (gb2312Str.empty()) return "";
    
    // 检查是否已经是 UTF-8（简单检查：如果包含多字节 UTF-8 字符特征）
    bool isUtf8 = true;
    for (size_t i = 0; i < gb2312Str.length(); ++i) {
      unsigned char c = gb2312Str[i];
      // UTF-8 多字节字符特征
      if (c >= 0x80) {
        // 如果是 GB2312 的单个中文字节（0xA1-0xF7），可能是乱码
        if (c >= 0xA1 && c <= 0xF7) {
          // 检查下一个字节
          if (i + 1 < gb2312Str.length()) {
            unsigned char c2 = gb2312Str[i + 1];
            if (c2 >= 0xA1 && c2 <= 0xFE) {
              // 符合 GB2312 双字节特征
              isUtf8 = false;
              break;
            }
          }
        }
      }
    }
    
    if (isUtf8) {
      return gb2312Str;  // 可能已经是 UTF-8，直接返回
    }
    
    // 使用 iconv 进行编码转换
    iconv_t cd = iconv_open("UTF-8", "GB2312");
    if (cd == (iconv_t)-1) {
      WarnL << "iconv_open failed for GB2312 to UTF-8";
      return gb2312Str;  // 转换失败，返回原字符串
    }
    
    size_t inLen = gb2312Str.length();
    size_t outLen = inLen * 4;  // UTF-8 可能更长，预留空间
    std::string result(outLen, '\0');
    
    char* inBuf = const_cast<char*>(gb2312Str.c_str());
    char* outBuf = &result[0];
    
    size_t inBytesLeft = inLen;
    size_t outBytesLeft = outLen;
    
    size_t ret = iconv(cd, &inBuf, &inBytesLeft, &outBuf, &outBytesLeft);
    iconv_close(cd);
    
    if (ret == (size_t)-1) {
      WarnL << "iconv conversion failed for GB2312 to UTF-8";
      return gb2312Str;  // 转换失败，返回原字符串
    }
    
    // 计算实际转换后的长度
    size_t convertedLen = outLen - outBytesLeft;
    result.resize(convertedLen);
    
    return result;
  }
}

bool sendCatalogQuery(const std::string& platformGbId, int sn) {
  if (!g_sip_endpt) {
    ErrorL << "SIP endpoint not initialized";
    return false;
  }
  
  std::string targetHost, signalHost;
  int targetPort = 5060, signalPort = 0;
  if (!loadPlatformSipRouting(platformGbId, targetHost, targetPort, signalHost, signalPort)) {
    ErrorL << "Platform not found: " << platformGbId;
    return false;
  }
  if (!contactUsableAsUriHost(targetHost)) {
    ErrorL << "Device contact address not available for: " << platformGbId
           << ", please ensure device has registered";
    return false;
  }

  // 获取本机地址和端口（用于 From/Contact）
  std::string localGbId = getSystemGbId();
  if (localGbId.empty()) {
    localGbId = "34020000002000000001";  // 默认本机ID
  }
  
  int localPort = 5060;  // 默认端口
  std::string sql = "SELECT COALESCE(signal_ip, '127.0.0.1'), COALESCE(signal_port, 5060) FROM gb_local_config LIMIT 1";
  std::string out = execPsql(sql.c_str());
  
  // 解析 IP 和端口
  size_t pipePos = out.find('|');
  std::string localHost;
  if (pipePos != std::string::npos) {
    localHost = trimString(out.substr(0, pipePos));
    std::string portStr = trimString(out.substr(pipePos + 1));
    if (!portStr.empty()) {
      localPort = std::atoi(portStr.c_str());
    }
  } else {
    localHost = trimString(out);
  }
  
  if (localHost.empty()) {
    localHost = "192.168.1.9";  // 默认本机IP
  }
  
  // 构建目标 URI（使用设备的 contact 地址）- 必须包含端口，符合 GB28181
  std::string targetUri = "sip:" + platformGbId + "@" + targetHost + ":" + std::to_string(targetPort);
  
  // 构建本地 URI（使用本机 signal_ip:port）- 必须包含端口，符合 GB28181
  std::string localUri = "sip:" + localGbId + "@" + localHost + ":" + std::to_string(localPort);
  
  /**
   * @note GB28181 Catalog Query XML 格式
   * @details 根据对比其他厂商实现，使用简化格式：
   *          - XML声明：<?xml version="1.0"?>（不加encoding）
   *          - 符合 GB/T 28181-2016 附录C 设备目录查询要求
   */
  std::string xmlBody = "<?xml version=\"1.0\"?>\r\n"
      "<Query>\r\n"
      "<CmdType>Catalog</CmdType>\r\n"
      "<SN>" + std::to_string(sn) + "</SN>\r\n"
      "<DeviceID>" + platformGbId + "</DeviceID>\r\n"
      "</Query>";
  
  // 创建内存池
  pj_pool_t* pool = pjsip_endpt_create_pool(g_sip_endpt, "catalog_query", 2048, 1024);
  if (!pool) {
    ErrorL << "Failed to create pool";
    return false;
  }
  
  // 目标 URI 字符串
  pj_str_t target_str = pj_str((char*)targetUri.c_str());
  
  // From URI 字符串
  pj_str_t from_str = pj_str((char*)localUri.c_str());
  
  // Call-ID
  char callIdBuf[64];
  snprintf(callIdBuf, sizeof(callIdBuf), "catalog_%d@%s", sn, localGbId.c_str());
  pj_str_t call_id = pj_str(callIdBuf);
  
  // CSeq
  static int cseq = 1;
  
  // 创建 MESSAGE 方法
  pj_str_t method_str = pj_str((char*)"MESSAGE");
  pjsip_method method;
  pjsip_method_init_np(&method, &method_str);
  
  // 解析 target URI
  pjsip_uri* target_uri = pjsip_parse_uri(pool, (char*)targetUri.c_str(), targetUri.length(), 0);
  if (!target_uri) {
    ErrorL << "Failed to parse target URI";
    pj_pool_release(pool);
    return false;
  }
  
  // 创建 From 头（name-addr 格式 <sip:user@host:port>，符合 GB28181 标准）
  pjsip_from_hdr* from_hdr = pjsip_from_hdr_create(pool);
  if (from_hdr) {
    // 创建 name-addr 结构（带尖括号 <> 的格式）
    pjsip_name_addr* from_name_addr = pjsip_name_addr_create(pool);
    if (from_name_addr) {
      // 创建内部的 sip URI
      pjsip_sip_uri* from_uri = pjsip_sip_uri_create(pool, PJ_FALSE);
      if (from_uri) {
        // 设置 user
        pj_strdup2(pool, &from_uri->user, (char*)localGbId.c_str());
        // 设置 host
        pj_strdup2(pool, &from_uri->host, (char*)localHost.c_str());
        // 设置 port
        from_uri->port = localPort;
        // 将 sip URI 赋值给 name-addr
        from_name_addr->uri = (pjsip_uri*)from_uri;
      }
      // 将 name-addr 赋值给 From 头
      from_hdr->uri = (pjsip_uri*)from_name_addr;
    }
  }

  // 创建 To 头（name-addr 格式 <sip:user@host:port>，符合 GB28181 标准）
  pjsip_to_hdr* to_hdr = pjsip_to_hdr_create(pool);
  if (to_hdr) {
    // 创建 name-addr 结构（带尖括号 <> 的格式）
    pjsip_name_addr* to_name_addr = pjsip_name_addr_create(pool);
    if (to_name_addr) {
      // 创建内部的 sip URI
      pjsip_sip_uri* to_uri = pjsip_sip_uri_create(pool, PJ_FALSE);
      if (to_uri) {
        // 设置 user
        pj_strdup2(pool, &to_uri->user, (char*)platformGbId.c_str());
        // 设置 host
        pj_strdup2(pool, &to_uri->host, (char*)targetHost.c_str());
        // 设置 port
        to_uri->port = targetPort;
        // 将 sip URI 赋值给 name-addr
        to_name_addr->uri = (pjsip_uri*)to_uri;
      }
      // 将 name-addr 赋值给 To 头
      to_hdr->uri = (pjsip_uri*)to_name_addr;
    }
  }

  /**
   * @note GB28181 Catalog查询不需要Contact头
   * @details 对比其他厂商实现（如GBXuean），Catalog查询消息中不包含Contact头
   *          RFC 3261中Contact头主要用于REGISTER和INVITE等需要建立联系的场景
   *          Catalog查询是单向查询，不需要建立长期联系，因此可以省略
   */
  // Contact头设为nullptr，不添加到请求中
  pjsip_contact_hdr* contact_hdr = nullptr;

  // 创建请求 - 使用构建好的 From/To 头，不添加 Contact 头
  pjsip_tx_data* tdata = nullptr;
  pj_status_t st = pjsip_endpt_create_request_from_hdr(g_sip_endpt,
                                                        &method,
                                                        target_uri,
                                                        from_hdr,
                                                        to_hdr,
                                                        contact_hdr,
                                                        nullptr,  // Call-ID will be generated
                                                        cseq++,
                                                        nullptr,  // No body yet
                                                        &tdata);
  
  if (st != PJ_SUCCESS) {
    ErrorL << "Failed to create SIP request: " << st;
    pj_pool_release(pool);
    return false;
  }

  presetTxDataUdpDestFromSignal(tdata, signalHost, signalPort, "【CATALOG】",
                                targetHost + ":" + std::to_string(targetPort));
  
  // 手动修正 Via 头（PJSIP 生成的可能不完整，需要添加正确地址和传输协议）
  pjsip_via_hdr* via = (pjsip_via_hdr*)pjsip_msg_find_hdr(tdata->msg, PJSIP_H_VIA, nullptr);
  if (via) {
    // 设置传输协议为 UDP
    pj_strdup2(pool, &via->transport, (char*)"UDP");
    // 设置正确的 Via 地址
    pj_strdup2(pool, &via->sent_by.host, (char*)localHost.c_str());
    via->sent_by.port = localPort;
    // 设置 branch
    char branch_buf[64];
    snprintf(branch_buf, sizeof(branch_buf), "z9hG4bK%d", sn);
    pj_strdup2(pool, &via->branch_param, branch_buf);
  }
  
  // 设置 Call-ID
  pjsip_cid_hdr* call_id_hdr = (pjsip_cid_hdr*)pjsip_msg_find_hdr(tdata->msg, PJSIP_H_CALL_ID, nullptr);
  if (call_id_hdr) {
    pj_strdup2(pool, &call_id_hdr->id, callIdBuf);
  }
  
  /**
   * @note 不添加 User-Agent 头
   * @details 对比其他厂商实现（如GBXuean），Catalog查询消息中不包含User-Agent头
   *          某些厂商平台可能对非标准header敏感
   */
  // User-Agent 头已省略

  // 设置 body - type="Application", subtype="MANSCDP+xml"
  // 注意：使用大写的 "Application" 以匹配某些厂商的实现（如GBXuean）
  // GB28181 标准要求使用 GB2312 编码
  pj_str_t body_str = pj_str((char*)xmlBody.c_str());
  pj_str_t type_str = pj_str((char*)"Application");  // 大写A，匹配厂商实现
  pj_str_t subtype_str = pj_str((char*)"MANSCDP+xml");
  tdata->msg->body = pjsip_msg_body_create(pool, &type_str, &subtype_str, &body_str);
  
  // 确保只有一个 Content-Type 头：如果已存在则删除旧的（PJSIP 有时会添加默认值）
  pjsip_ctype_hdr* existing_ct = (pjsip_ctype_hdr*)pjsip_msg_find_hdr(tdata->msg, PJSIP_H_CONTENT_TYPE, nullptr);
  if (existing_ct && existing_ct->next) {
    // 如果有多个 Content-Type 头，只保留最后一个（我们创建的）
    pj_list_erase(existing_ct);
  }
  
  // 打印完整的 SIP 消息用于调试
  char msg_buf[2048];
  pj_ssize_t msg_len = pjsip_msg_print(tdata->msg, msg_buf, sizeof(msg_buf));
  if (msg_len > 0) {
    InfoL << "【CATALOG SIP消息】准备发送:\n" << std::string(msg_buf, msg_len);
  }
  
  // 发送请求 - 使用 stateless 方式（不需要状态模块）
  st = pjsip_endpt_send_request_stateless(g_sip_endpt, tdata, nullptr, nullptr);
  if (st != PJ_SUCCESS) {
    ErrorL << "Failed to send Catalog request: " << st;
    pj_pool_release(pool);
    return false;
  }
  
  // 标记查询进行中
  g_queryInProgress[platformGbId] = true;
  
  InfoL << "【CATALOG已发送】R-URI=" << targetUri << " SN=" << sn << " From=" << localUri
        << " Contact=" << targetHost << ":" << targetPort
        << (signalHost.empty() ? "" : (" UDP目的(signal_src)=" + signalHost + ":" + std::to_string(signalPort)));
  
  pj_pool_release(pool);
  return true;
}

/**
 * @brief 发送国标录像目录查询 MESSAGE（CmdType=RecordInfo）
 * @details DeviceID 为通道/摄像机国标 ID；StartTime/EndTime 为 GB28181 常用格式（日期与时间之间为 T）
 */
bool sendRecordInfoQuery(const std::string& platformGbId,
                         const std::string& channelId,
                         const std::string& startTimeGb,
                         const std::string& endTimeGb,
                         int sn) {
  if (!g_sip_endpt) {
    ErrorL << "【RecordInfo】SIP endpoint not initialized";
    return false;
  }
  std::string targetHost, signalHost;
  int targetPort = 5060, signalPort = 0;
  if (!loadPlatformSipRouting(platformGbId, targetHost, targetPort, signalHost, signalPort)) {
    ErrorL << "【RecordInfo】Platform not found: " << platformGbId;
    return false;
  }
  if (!contactUsableAsUriHost(targetHost)) {
    ErrorL << "【RecordInfo】Contact unavailable: " << platformGbId;
    return false;
  }

  std::string localGbId = getSystemGbId();
  if (localGbId.empty()) localGbId = "34020000002000000001";

  int localPort = 5060;
  std::string sql = "SELECT COALESCE(signal_ip, '127.0.0.1'), COALESCE(signal_port, 5060) FROM gb_local_config LIMIT 1";
  std::string out = execPsql(sql.c_str());
  std::string localHost = "127.0.0.1";
  if (!out.empty()) {
    size_t pipePos = out.find('|');
    if (pipePos != std::string::npos) {
      localHost = trimString(out.substr(0, pipePos));
      std::string portStr = trimString(out.substr(pipePos + 1));
      if (!portStr.empty()) localPort = std::atoi(portStr.c_str());
    } else {
      localHost = trimString(out);
    }
  }
  if (localHost.empty()) localHost = "127.0.0.1";

  std::string targetUri = "sip:" + platformGbId + "@" + targetHost + ":" + std::to_string(targetPort);
  std::string localUri = "sip:" + localGbId + "@" + localHost + ":" + std::to_string(localPort);

  std::string xmlBody = std::string("<?xml version=\"1.0\"?>\r\n") + "<Query>\r\n" + "<CmdType>RecordInfo</CmdType>\r\n" +
                        "<SN>" + std::to_string(sn) + "</SN>\r\n" + "<DeviceID>" + channelId + "</DeviceID>\r\n" +
                        "<StartTime>" + startTimeGb + "</StartTime>\r\n" + "<EndTime>" + endTimeGb +
                        "</EndTime>\r\n" + "<Type>all</Type>\r\n" + "</Query>";

  pj_pool_t* pool = pjsip_endpt_create_pool(g_sip_endpt, "recordinfo_q", 2048, 1024);
  if (!pool) {
    ErrorL << "【RecordInfo】Failed to create pool";
    return false;
  }

  char callIdBuf[80];
  snprintf(callIdBuf, sizeof(callIdBuf), "recinfo_%d@%s", sn, localGbId.c_str());
  pj_str_t call_id = pj_str(callIdBuf);

  static int cseqRi = 1;
  pj_str_t method_str = pj_str((char*)"MESSAGE");
  pjsip_method method;
  pjsip_method_init_np(&method, &method_str);

  pjsip_uri* target_uri = pjsip_parse_uri(pool, (char*)targetUri.c_str(), targetUri.length(), 0);
  if (!target_uri) {
    ErrorL << "【RecordInfo】Failed to parse target URI";
    pj_pool_release(pool);
    return false;
  }

  pjsip_from_hdr* from_hdr = pjsip_from_hdr_create(pool);
  if (from_hdr) {
    pjsip_name_addr* from_name_addr = pjsip_name_addr_create(pool);
    if (from_name_addr) {
      pjsip_sip_uri* from_uri = pjsip_sip_uri_create(pool, PJ_FALSE);
      if (from_uri) {
        pj_strdup2(pool, &from_uri->user, (char*)localGbId.c_str());
        pj_strdup2(pool, &from_uri->host, (char*)localHost.c_str());
        from_uri->port = localPort;
        from_name_addr->uri = (pjsip_uri*)from_uri;
      }
      from_hdr->uri = (pjsip_uri*)from_name_addr;
    }
  }

  pjsip_to_hdr* to_hdr = pjsip_to_hdr_create(pool);
  if (to_hdr) {
    pjsip_name_addr* to_name_addr = pjsip_name_addr_create(pool);
    if (to_name_addr) {
      pjsip_sip_uri* to_uri = pjsip_sip_uri_create(pool, PJ_FALSE);
      if (to_uri) {
        pj_strdup2(pool, &to_uri->user, (char*)platformGbId.c_str());
        pj_strdup2(pool, &to_uri->host, (char*)targetHost.c_str());
        to_uri->port = targetPort;
        to_name_addr->uri = (pjsip_uri*)to_uri;
      }
      to_hdr->uri = (pjsip_uri*)to_name_addr;
    }
  }

  pjsip_tx_data* tdata = nullptr;
  pj_status_t st = pjsip_endpt_create_request_from_hdr(g_sip_endpt, &method, target_uri, from_hdr, to_hdr, nullptr,
                                                       nullptr, cseqRi++, nullptr, &tdata);
  if (st != PJ_SUCCESS) {
    ErrorL << "【RecordInfo】Failed to create SIP request: " << st;
    pj_pool_release(pool);
    return false;
  }

  presetTxDataUdpDestFromSignal(tdata, signalHost, signalPort, "【RECORDINFO】",
                                targetHost + ":" + std::to_string(targetPort));

  pjsip_via_hdr* via = (pjsip_via_hdr*)pjsip_msg_find_hdr(tdata->msg, PJSIP_H_VIA, nullptr);
  if (via) {
    pj_strdup2(pool, &via->transport, (char*)"UDP");
    pj_strdup2(pool, &via->sent_by.host, (char*)localHost.c_str());
    via->sent_by.port = localPort;
    char branch_buf[64];
    snprintf(branch_buf, sizeof(branch_buf), "z9hG4bKri%d", sn);
    pj_strdup2(pool, &via->branch_param, branch_buf);
  }

  pjsip_cid_hdr* call_id_hdr = (pjsip_cid_hdr*)pjsip_msg_find_hdr(tdata->msg, PJSIP_H_CALL_ID, nullptr);
  if (call_id_hdr) pj_strdup2(pool, &call_id_hdr->id, callIdBuf);

  pj_str_t body_str = pj_str((char*)xmlBody.c_str());
  pj_str_t type_str = pj_str((char*)"Application");
  pj_str_t subtype_str = pj_str((char*)"MANSCDP+xml");
  tdata->msg->body = pjsip_msg_body_create(pool, &type_str, &subtype_str, &body_str);

  pjsip_ctype_hdr* existing_ct = (pjsip_ctype_hdr*)pjsip_msg_find_hdr(tdata->msg, PJSIP_H_CONTENT_TYPE, nullptr);
  if (existing_ct && existing_ct->next) pj_list_erase(existing_ct);

  char msg_buf[2048];
  pj_ssize_t msg_len = pjsip_msg_print(tdata->msg, msg_buf, sizeof(msg_buf));
  if (msg_len > 0) InfoL << "【RECORDINFO SIP】\n" << std::string(msg_buf, msg_len);

  st = pjsip_endpt_send_request_stateless(g_sip_endpt, tdata, nullptr, nullptr);
  if (st != PJ_SUCCESS) {
    ErrorL << "【RecordInfo】send failed: " << st;
    pj_pool_release(pool);
    return false;
  }

  InfoL << "【RECORDINFO 已发送】SN=" << sn << " channel=" << channelId << " R-URI=" << targetUri;
  pj_pool_release(pool);
  return true;
}

/**
 * @brief RecordInfo MESSAGE 发送失败时通知 HTTP 阻塞等待（否则仅 35s 超时且无明确原因）
 */
static void failRecordInfoWaitBySn(int sn, const std::string& errMsg) {
  std::shared_ptr<RecordInfoWaitSlot> slot;
  {
    std::lock_guard<std::mutex> lk(g_recordInfoWaitMutex);
    auto it = g_recordInfoWaitBySn.find(sn);
    if (it == g_recordInfoWaitBySn.end()) return;
    slot = it->second;
  }
  execPsqlCommand("UPDATE replay_tasks SET status='failed',updated_at=CURRENT_TIMESTAMP WHERE id=" +
                  std::to_string(slot->taskDbId));
  {
    std::lock_guard<std::mutex> lk(slot->mtx);
    slot->success = false;
    slot->error = errMsg;
    slot->done = true;
    slot->cv.notify_all();
  }
  std::lock_guard<std::mutex> wlk(g_recordInfoWaitMutex);
  g_recordInfoWaitBySn.erase(sn);
}

void processPendingRecordInfoQueries() {
  std::vector<PendingRecordInfoQuery> batch;
  {
    std::lock_guard<std::mutex> lk(g_recordInfoQueriesMutex);
    batch.swap(g_pendingRecordInfoQueries);
  }
  for (const auto& q : batch) {
    InfoL << "【RecordInfo】PJSIP 线程发送 SN=" << q.sn << " platform=" << q.platformGbId
          << " channel=" << q.channelId;
    if (!sendRecordInfoQuery(q.platformGbId, q.channelId, q.startGb, q.endGb, q.sn)) {
      failRecordInfoWaitBySn(q.sn,
                             "RecordInfo 未发出（平台路由失败：请查库 device_platforms.contact 与 gb_id，或见日志 "
                             "【RecordInfo】Platform not found / Contact unavailable）");
    }
  }
}

bool enqueueRecordInfoQuery(const std::string& platformGbId,
                            const std::string& channelId,
                            const std::string& startTimeGb,
                            const std::string& endTimeGb,
                            int sn) {
  if (platformGbId.empty() || channelId.empty() || sn <= 0) return false;
  std::lock_guard<std::mutex> lk(g_recordInfoQueriesMutex);
  g_pendingRecordInfoQueries.push_back(PendingRecordInfoQuery{platformGbId, channelId, startTimeGb, endTimeGb, sn});
  return true;
}

namespace {

struct ReplaySegRow {
  std::string segmentId;
  std::string startSql;
  std::string endSql;
  int durationSec;
};

/**
 * @brief 将国标 RecordInfo 中的时间统一为「本地墙钟」YYYY-MM-DD HH:mm:ss（去 T、去 +08/Z）
 * @details 混用带时区与无时区时，直接 ::timestamptz 会导致起止颠倒；入库前统一再按 Asia/Shanghai 写入。
 */
static std::string normalizeGbRecordTime(const std::string& raw) {
  std::string s = trimString(raw);
  for (char& c : s)
    if (c == 'T') c = ' ';
  static const char* kSuffixes[] = {"+08:00", "+08", "Z", "+00:00"};
  for (const char* suf : kSuffixes) {
    size_t len = strlen(suf);
    if (s.size() >= len && s.compare(s.size() - len, len, suf) == 0) {
      s.resize(s.size() - len);
      while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
      break;
    }
  }
  return trimString(s);
}

bool parseRecordInfoItems(const std::string& body, std::vector<ReplaySegRow>& out) {
  out.clear();
  size_t pos = 0;
  while (true) {
    size_t itemStart = body.find("<Item", pos);
    if (itemStart == std::string::npos) break;
    size_t tagEnd = body.find(">", itemStart);
    if (tagEnd == std::string::npos) break;
    size_t endPos = body.find("</Item>", tagEnd);
    if (endPos == std::string::npos) break;
    std::string itemXml = body.substr(tagEnd + 1, endPos - tagEnd - 1);
    std::string st = extractXmlTag(itemXml, "StartTime");
    std::string et = extractXmlTag(itemXml, "EndTime");
    std::string fp = extractXmlTag(itemXml, "FilePath");
    std::string name = extractXmlTag(itemXml, "Name");
    if (st.empty() || et.empty()) {
      pos = endPos + 7;
      continue;
    }
    ReplaySegRow row;
    row.startSql = normalizeGbRecordTime(st);
    row.endSql = normalizeGbRecordTime(et);
    if (row.startSql.empty() || row.endSql.empty()) {
      pos = endPos + 7;
      continue;
    }
    if (row.startSql > row.endSql) std::swap(row.startSql, row.endSql);

    row.segmentId = !fp.empty() ? fp : (row.startSql + "|" + row.endSql);
    if (row.segmentId.size() > 60) row.segmentId = row.segmentId.substr(0, 60);
    row.durationSec = 0;
    struct tm tms{}, tme{};
    if (strptime(row.startSql.c_str(), "%Y-%m-%d %H:%M:%S", &tms) != nullptr &&
        strptime(row.endSql.c_str(), "%Y-%m-%d %H:%M:%S", &tme) != nullptr) {
      time_t a = timegm(&tms);
      time_t b = timegm(&tme);
      if (a > 0 && b >= a) row.durationSec = static_cast<int>(b - a);
    }
    (void)name;
    out.push_back(std::move(row));
    pos = endPos + 7;
  }
  return true;
}

/** 同一 RecordInfo 响应内可能重复出现相同 Item，按起止时间去重 */
static void dedupeReplaySegRows(std::vector<ReplaySegRow>& items) {
  std::unordered_set<std::string> seen;
  std::vector<ReplaySegRow> out;
  out.reserve(items.size());
  for (auto& r : items) {
    std::string key = r.startSql;
    key.push_back('\x01');
    key += r.endSql;
    if (seen.insert(std::move(key)).second) {
      out.push_back(std::move(r));
    }
  }
  items.swap(out);
}

/**
 * @brief 该摄像头是否已存在相同起止时间的录像片段（跨多次检索、多 replay_tasks）
 */
static bool replaySegmentExistsForCamera(const std::string& cameraDbId,
                                         const std::string& startSql,
                                         const std::string& endSql) {
  if (cameraDbId.empty()) return false;
  for (char c : cameraDbId) {
    if (c < '0' || c > '9') return false;
  }
  std::string sql =
      "SELECT COUNT(*) FROM replay_segments s INNER JOIN replay_tasks t ON t.id = s.task_id "
      "WHERE t.camera_id = " +
      cameraDbId + " AND "
                      "s.start_time = ('" +
      gb::escapeSqlString(startSql) + "'::timestamp AT TIME ZONE 'Asia/Shanghai') AND "
                                      "s.end_time = ('" +
      gb::escapeSqlString(endSql) + "'::timestamp AT TIME ZONE 'Asia/Shanghai')";
  std::string out = execPsql(sql.c_str());
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
    out.pop_back();
  }
  long long n = 0;
  if (!out.empty()) {
    try {
      n = std::stoll(out);
    } catch (...) {
      n = 0;
    }
  }
  return n > 0;
}

static std::string sqlTimeToGbXml(const std::string& sqlTime) {
  std::string s = sqlTime;
  for (char& c : s)
    if (c == ' ') {
      c = 'T';
      break;
    }
  return s;
}

}  // namespace

void handleRecordInfoMessage(const std::string& /*fromPlatformGbId*/, const std::string& body) {
  int sn = extractXmlTagInt(body, "SN", 0);
  std::shared_ptr<RecordInfoWaitSlot> slot;
  {
    std::lock_guard<std::mutex> lk(g_recordInfoWaitMutex);
    auto it = g_recordInfoWaitBySn.find(sn);
    if (it == g_recordInfoWaitBySn.end()) {
      InfoL << "【RecordInfo】无等待中的 HTTP 检索 SN=" << sn;
      return;
    }
    slot = it->second;
  }

  std::string result = extractXmlTag(body, "Result");
  if (result.empty()) result = extractXmlTag(body, "ResultCode");
  bool ok = true;
  if (!result.empty()) {
    std::string rl = result;
    for (auto& c : rl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (rl == "error") ok = false;
    else if (rl == "ok" || result == "0" || result == "200")
      ok = true;
  }

  std::vector<ReplaySegRow> items;
  parseRecordInfoItems(body, items);
  dedupeReplaySegRows(items);

  if (!ok && items.empty()) {
    std::lock_guard<std::mutex> lk(slot->mtx);
    slot->success = false;
    slot->error = "平台返回录像查询失败: " + result;
    slot->done = true;
    slot->cv.notify_all();
    std::lock_guard<std::mutex> wlk(g_recordInfoWaitMutex);
    g_recordInfoWaitBySn.erase(sn);
    execPsqlCommand("UPDATE replay_tasks SET status='failed',updated_at=CURRENT_TIMESTAMP WHERE id=" +
                    std::to_string(slot->taskDbId));
    return;
  }

  std::string camPk = slot->cameraDbId.empty() ? std::string() : slot->cameraDbId;
  for (const auto& it : items) {
    if (replaySegmentExistsForCamera(camPk, it.startSql, it.endSql)) {
      InfoL << "【RecordInfo】跳过已存在片段 camera=" << slot->cameraId << " " << it.startSql << " ~ " << it.endSql;
      continue;
    }
    std::string ins =
        "INSERT INTO replay_segments (task_id, segment_id, start_time, end_time, duration_seconds, downloadable) VALUES (" +
        std::to_string(slot->taskDbId) + ",'" + gb::escapeSqlString(it.segmentId) + "'," + "(" + "'" +
        gb::escapeSqlString(it.startSql) + "'::timestamp AT TIME ZONE 'Asia/Shanghai'),(" + "'" +
        gb::escapeSqlString(it.endSql) + "'::timestamp AT TIME ZONE 'Asia/Shanghai')," +
        std::to_string(it.durationSec) + ",true)";
    execPsqlCommand(ins);
  }

  execPsqlCommand("UPDATE replay_tasks SET status='completed',updated_at=CURRENT_TIMESTAMP WHERE id=" +
                  std::to_string(slot->taskDbId));

  {
    std::lock_guard<std::mutex> lk(slot->mtx);
    slot->success = true;
    slot->error.clear();
    slot->done = true;
    slot->cv.notify_all();
  }
  std::lock_guard<std::mutex> wlk(g_recordInfoWaitMutex);
  g_recordInfoWaitBySn.erase(sn);
}

static bool isLikelyCameraDbPathKey(const std::string& s) {
  if (s.empty() || s.size() > 12) return false;
  for (unsigned char c : s) {
    if (!std::isdigit(c)) return false;
  }
  return true;
}

bool resolveCameraRowByPathSegment(const std::string& segment,
                                   const std::string& platformGbIdOpt,
                                   std::string& outDbId,
                                   std::string& outDeviceGbId,
                                   std::string& outPlatformGbId) {
  outDbId.clear();
  outDeviceGbId.clear();
  outPlatformGbId.clear();
  if (segment.empty() || segment.find('/') != std::string::npos) return false;

  auto parseOneLine = [](const std::string& out) -> bool {
    return !trimSqlField(out).empty();
  };

  if (isLikelyCameraDbPathKey(segment)) {
    std::string sql = "SELECT c.id::text, c.device_gb_id, COALESCE(NULLIF(TRIM(c.platform_gb_id),''), p.gb_id) "
                      "FROM cameras c LEFT JOIN device_platforms p ON p.id = c.platform_id WHERE c.id = " +
                      segment + " LIMIT 1";
    std::string out = execPsql(sql.c_str());
    if (!out.empty()) {
      size_t nl = out.find('\n');
      std::string line = nl == std::string::npos ? out : out.substr(0, nl);
      std::vector<std::string> cols;
      std::string cur;
      for (char ch : line) {
        if (ch == '|') {
          cols.push_back(trimSqlField(cur));
          cur.clear();
        } else {
          cur += ch;
        }
      }
      cols.push_back(trimSqlField(cur));
      if (cols.size() >= 3 && !cols[0].empty() && !cols[1].empty()) {
        outDbId = cols[0];
        outDeviceGbId = cols[1];
        outPlatformGbId = cols[2];
        return true;
      }
    }
  }

  std::string escDev = gb::escapeSqlString(segment);
  std::string platFilter;
  if (!platformGbIdOpt.empty()) {
    std::string peg = gb::escapeSqlString(platformGbIdOpt);
    platFilter = " AND (NULLIF(TRIM(c.platform_gb_id),'') = '" + peg + "' OR p.gb_id = '" + peg + "')";
  }
  std::string sql2 = "SELECT c.id::text, c.device_gb_id, COALESCE(NULLIF(TRIM(c.platform_gb_id),''), p.gb_id) "
                     "FROM cameras c LEFT JOIN device_platforms p ON p.id = c.platform_id WHERE c.device_gb_id = '" +
                     escDev + "'" + platFilter;
  std::string out2 = execPsql(sql2.c_str());
  if (out2.empty()) return false;
  std::vector<std::string> lines;
  {
    std::istringstream iss(out2);
    std::string ln;
    while (std::getline(iss, ln)) {
      if (!trimSqlField(ln).empty()) lines.push_back(ln);
    }
  }
  if (lines.empty()) return false;
  if (lines.size() > 1 && platformGbIdOpt.empty()) return false;

  std::string line = lines[0];
  std::vector<std::string> cols;
  std::string cur;
  for (char ch : line) {
    if (ch == '|') {
      cols.push_back(trimSqlField(cur));
      cur.clear();
    } else {
      cur += ch;
    }
  }
  cols.push_back(trimSqlField(cur));
  if (cols.size() < 3 || cols[0].empty() || cols[1].empty()) return false;
  outDbId = cols[0];
  outDeviceGbId = cols[1];
  outPlatformGbId = cols[2];
  return true;
}

bool runRecordInfoSearchBlocking(const std::string& cameraId,
                                 const std::string& platformGbIdOpt,
                                 const std::string& startTime,
                                 const std::string& endTime,
                                 std::string& outError) {
  outError.clear();
  if (!g_sip_endpt) {
    outError = "SIP 未初始化";
    return false;
  }
  std::string dbId;
  std::string deviceGbId;
  std::string platformGbFromRow;
  if (!resolveCameraRowByPathSegment(cameraId, platformGbIdOpt, dbId, deviceGbId, platformGbFromRow)) {
    outError = "摄像头不存在或 deviceGbId 在多平台重复且未传 platformGbId";
    return false;
  }
  std::string platformGbId = platformGbIdOpt;
  if (platformGbId.empty()) platformGbId = platformGbFromRow;
  if (platformGbId.empty()) {
    WarnL << "【RecordInfo】无法解析平台国标 ID：camera=" << cameraId;
    outError = "缺少 platformGbId 或摄像头未关联平台";
    return false;
  }
  InfoL << "【RecordInfo】HTTP 检索开始 cameraDbId=" << dbId << " deviceGbId=" << deviceGbId
        << " platformGbId=" << platformGbId << " range=" << startTime << " ~ " << endTime;

  std::string escSt = gb::escapeSqlString(startTime);
  std::string escEt = gb::escapeSqlString(endTime);

  int sn = g_recordInfoSnCounter.fetch_add(1, std::memory_order_relaxed);
  if (sn <= 0) sn = 1;

  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
  std::string taskId = "ri-" + std::to_string(sn) + "-" + std::to_string(ms);

  std::string ins = "INSERT INTO replay_tasks (task_id, camera_id, start_time, end_time, status) VALUES ('" +
                    gb::escapeSqlString(taskId) + "'," + dbId + ",'" + escSt + "'::timestamptz,'" + escEt +
                    "'::timestamptz,'searching') RETURNING id";
  std::string idOut = execPsql(ins.c_str());
  if (idOut.empty()) {
    outError = "创建检索任务失败";
    return false;
  }
  size_t nl = idOut.find('\n');
  if (nl != std::string::npos) idOut = idOut.substr(0, nl);
  int64_t taskDbId = std::atoll(idOut.c_str());
  if (taskDbId <= 0) {
    outError = "无效的 task id";
    return false;
  }

  auto slot = std::make_shared<RecordInfoWaitSlot>();
  slot->taskDbId = taskDbId;
  slot->cameraId = deviceGbId;
  slot->cameraDbId = dbId;
  {
    std::lock_guard<std::mutex> lk(g_recordInfoWaitMutex);
    g_recordInfoWaitBySn[sn] = slot;
  }

  std::string startGb = sqlTimeToGbXml(startTime);
  std::string endGb = sqlTimeToGbXml(endTime);
  {
    std::lock_guard<std::mutex> lk(g_recordInfoQueriesMutex);
    g_pendingRecordInfoQueries.push_back(
        PendingRecordInfoQuery{platformGbId, deviceGbId, startGb, endGb, sn});
  }

  std::unique_lock<std::mutex> ul(slot->mtx);
  bool notified = slot->cv.wait_for(ul, std::chrono::seconds(35), [&] { return slot->done; });
  if (!notified) {
    outError = "录像检索超时";
    execPsqlCommand("UPDATE replay_tasks SET status='failed',updated_at=CURRENT_TIMESTAMP WHERE id=" +
                    std::to_string(taskDbId));
    std::lock_guard<std::mutex> wlk(g_recordInfoWaitMutex);
    g_recordInfoWaitBySn.erase(sn);
    return false;
  }
  if (!slot->success) {
    outError = slot->error.empty() ? "录像检索失败" : slot->error;
    return false;
  }
  return true;
}

bool parseCatalogResponse(const std::string& body, 
                          std::vector<CameraInfo>& cameras,
                          int& totalSum) {
  cameras.clear();
  
  // 检查是否是 Notify
  if (body.find("<CmdType>Notify</CmdType>") == std::string::npos &&
      body.find("<CmdType>Catalog</CmdType>") == std::string::npos) {
    return false;
  }
  
  // 提取总数
  totalSum = extractXmlTagInt(body, "SumNum", 0);
  
  // 查找所有 DeviceList Item（支持带属性的标签如 <Item Index="X">）
  size_t pos = 0;
  while (true) {
    // 查找 <Item 开头（可能带属性）
    size_t itemStart = body.find("<Item", pos);
    if (itemStart == std::string::npos) break;
    
    // 查找 > 结束标签开始
    size_t tagEnd = body.find(">", itemStart);
    if (tagEnd == std::string::npos) break;
    
    // 查找 </Item> 结束标签
    size_t endPos = body.find("</Item>", tagEnd);
    if (endPos == std::string::npos) break;
    
    // 提取 Item 内容（从 > 之后到 </Item> 之前）
    std::string itemXml = body.substr(tagEnd + 1, endPos - tagEnd - 1);
    
    CameraInfo cam;
    cam.deviceId = extractXmlTag(itemXml, "DeviceID");
    cam.name = extractXmlTag(itemXml, "Name");
    cam.manufacturer = extractXmlTag(itemXml, "Manufacturer");
    cam.model = extractXmlTag(itemXml, "Model");
    cam.owner = extractXmlTag(itemXml, "Owner");
    cam.civilCode = extractXmlTag(itemXml, "CivilCode");
    cam.address = extractXmlTag(itemXml, "Address");
    cam.parental = extractXmlTagInt(itemXml, "Parental", 0);
    cam.parentId = extractXmlTag(itemXml, "ParentID");
    cam.safetyWay = extractXmlTagInt(itemXml, "SafetyWay", 0);
    cam.registerWay = extractXmlTagInt(itemXml, "RegisterWay", 1);
    cam.secrecy = extractXmlTag(itemXml, "Secrecy");
    if (cam.secrecy.empty()) cam.secrecy = "0";
    cam.status = extractXmlTag(itemXml, "Status");
    
    // 只添加有效的设备（有 DeviceID）
    if (!cam.deviceId.empty()) {
      cameras.push_back(cam);
      InfoL << "【CATALOG PARSED】DeviceID=" << cam.deviceId << " Name=" << cam.name;
    }
    
    pos = endPos + 7;
  }
  
  return !cameras.empty() || totalSum > 0;
}

int saveCamerasToDb(int platformId, const std::string& platformGbId,
                    const std::vector<CameraInfo>& cameras) {
  int savedCount = 0;
  const std::string escPlatGb = gb::escapeSqlString(platformGbId);

  for (const auto& cam : cameras) {
    const std::string escDev = gb::escapeSqlString(cam.deviceId);
    std::string checkSql = "SELECT id::text FROM cameras WHERE platform_gb_id = '" + escPlatGb +
                           "' AND device_gb_id = '" + escDev + "'";
    std::string existing = execPsql(checkSql.c_str());
    if (!existing.empty()) {
      size_t nl = existing.find('\n');
      if (nl != std::string::npos) existing = existing.substr(0, nl);
      existing = trimSqlField(existing);
    }

    std::string deviceName = cam.name.empty() ? ("Camera_" + cam.deviceId) : cam.name;
    std::string onlineStr = (cam.status == "ON" || cam.status == "on") ? "true" : "false";
    std::string manufacturer = gb::escapeSqlString(cam.manufacturer);
    std::string model = gb::escapeSqlString(cam.model);
    std::string owner = gb::escapeSqlString(cam.owner);
    std::string civilCode = gb::escapeSqlString(cam.civilCode);
    std::string address = gb::escapeSqlString(cam.address);
    std::string parentId = gb::escapeSqlString(cam.parentId);
    std::string secrecy = gb::escapeSqlString(cam.secrecy);
    std::string parental = std::to_string(cam.parental);
    std::string safetyWay = std::to_string(cam.safetyWay);
    std::string registerWay = std::to_string(cam.registerWay);

    if (existing.empty()) {
      std::string insertSql =
          "INSERT INTO cameras (device_gb_id, name, platform_id, platform_gb_id, online, "
          "manufacturer, model, owner, civil_code, address, parental, parent_id, safety_way, register_way, secrecy) "
          "VALUES ('" +
          escDev + "', '" + gb::escapeSqlString(deviceName) + "', " + std::to_string(platformId) + ", '" + escPlatGb +
          "', " + onlineStr + ", " + "'" + manufacturer + "', '" + model + "', '" + owner + "', '" + civilCode +
          "', '" + address + "', " + parental + ", '" + parentId + "', " + safetyWay + ", " + registerWay + ", '" +
          secrecy + "')";

      if (execPsqlCommand(insertSql)) {
        savedCount++;
      } else {
        ErrorL << "Failed to insert camera: " << cam.deviceId << " SQL: " << insertSql.substr(0, 200);
      }
    } else {
      std::string updateSql = "UPDATE cameras SET "
                              "name = '" +
                              gb::escapeSqlString(deviceName) + "', " + "platform_id = " + std::to_string(platformId) +
                              ", " + "platform_gb_id = '" + escPlatGb + "', " + "online = " + onlineStr + ", " +
                              "manufacturer = '" + manufacturer + "', " + "model = '" + model + "', " + "owner = '" +
                              owner + "', " + "civil_code = '" + civilCode + "', " + "address = '" + address + "', " +
                              "parental = " + parental + ", " + "parent_id = '" + parentId + "', " + "safety_way = " +
                              safetyWay + ", " + "register_way = " + registerWay + ", " + "secrecy = '" + secrecy +
                              "', " + "updated_at = CURRENT_TIMESTAMP " + "WHERE id = " + existing;

      if (execPsqlCommand(updateSql)) {
        savedCount++;
      } else {
        ErrorL << "Failed to update camera: " << cam.deviceId;
      }
    }
  }

  InfoL << "Saved/Updated " << savedCount << " cameras to database for platform " << platformGbId;
  return savedCount;
}

void setCatalogResponseCallback(CatalogResponseCallback callback) {
  g_catalogCallback = callback;
}

bool isCatalogQueryInProgress(const std::string& platformGbId) {
  auto it = g_queryInProgress.find(platformGbId);
  if (it != g_queryInProgress.end()) {
    return it->second;
  }
  return false;
}

bool sendCatalogQueryAsync(const std::string& platformGbId, int sn, bool forceEnqueue) {
  std::lock_guard<std::mutex> lock(g_queriesMutex);

  if (!forceEnqueue && isCatalogQueryInProgress(platformGbId)) {
    InfoL << "Catalog query already in flight for " << platformGbId << ", skip enqueue";
    return true;
  }
  
  // 检查是否已有相同平台的查询在等待
  if (!forceEnqueue) {
    for (const auto& q : g_pendingQueries) {
      if (q.platformGbId == platformGbId) {
        InfoL << "Catalog query already pending for " << platformGbId;
        return true;  // 已经在队列中
      }
    }
  }
  
  PendingCatalogQuery query{platformGbId, sn};
  g_pendingQueries.push_back(query);
  InfoL << "Catalog query queued for " << platformGbId << " (SN=" << sn
        << ", force=" << (forceEnqueue ? "true" : "false") << ")";
  return true;
}

void processPendingCatalogQueries() {
  std::vector<PendingCatalogQuery> queries;
  {
    std::lock_guard<std::mutex> lock(g_queriesMutex);
    queries.swap(g_pendingQueries);
  }
  
  for (const auto& q : queries) {
    sendCatalogQuery(q.platformGbId, q.sn);
  }
}

// 内部函数：当收到 Catalog Notify 时调用
void handleCatalogNotify(const std::string& platformGbId, const std::string& body) {
  std::vector<CameraInfo> cameras;
  int totalSum = 0;

  if (parseCatalogResponse(body, cameras, totalSum)) {
    InfoL << "Received Catalog response from " << platformGbId
          << ": " << cameras.size() << " devices (total=" << totalSum << ")";

    /**
     * @note 从 XML 中的 DeviceID 获取目标平台ID
     * @details Catalog 响应中的 DeviceID 是被查询的平台ID
     *          例如：<DeviceID>42000000112007000011</DeviceID>
     *          我们使用这个 ID 来关联平台，而不是 From 头中的发送方ID
     */
    std::string targetPlatformGbId = extractXmlTag(body, "DeviceID");
    if (targetPlatformGbId.empty()) {
      targetPlatformGbId = platformGbId;  // 回退到 From 中的ID
    }

    // 查询平台数据库ID
    std::string sql = "SELECT id FROM device_platforms WHERE gb_id = '" +
        gb::escapeSqlString(targetPlatformGbId) + "'";
    std::string idStr = execPsql(sql.c_str());
    int platformId = 0;
    if (!idStr.empty()) {
      platformId = std::atoi(idStr.c_str());
    }

    InfoL << "【CATALOG SAVE】TargetPlatform=" << targetPlatformGbId
          << " PlatformId=" << platformId << " Cameras=" << cameras.size();

    // 保存到数据库
    if (platformId > 0) {
      int saved = saveCamerasToDb(platformId, targetPlatformGbId, cameras);
      InfoL << "【CATALOG SAVED】" << saved << " cameras to platform " << targetPlatformGbId;
    } else {
      WarnL << "【CATALOG SKIP】Platform not found: " << targetPlatformGbId;
    }

    // 调用回调
    if (g_catalogCallback) {
      g_catalogCallback(targetPlatformGbId, cameras, totalSum);
    }

    // 清除查询进行中标记（使用原始 platformGbId）
    g_queryInProgress[platformGbId] = false;
  }
}

// ============================================================================
// 新版目录树功能实现（支持设备、目录、行政区域三种节点类型）
// ============================================================================

namespace {
  // Catalog Session 管理（用于处理分页上报）
  std::map<std::string, std::unique_ptr<CatalogSession>> g_catalogSessions;
  std::map<std::string, std::chrono::steady_clock::time_point> g_recentCompletedCatalogSessions;
  std::mutex g_sessionsMutex;
  
  // 生成 session key
  std::string makeSessionKey(const std::string& platformGbId, int sn) {
    return platformGbId + "_" + std::to_string(sn);
  }

  bool isRecentlyCompletedSession(const std::string& sessionKey, int dedupeSeconds = 60) {
    auto now = std::chrono::steady_clock::now();
    auto it = g_recentCompletedCatalogSessions.find(sessionKey);
    if (it == g_recentCompletedCatalogSessions.end()) {
      return false;
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count();
    if (elapsed > dedupeSeconds) {
      g_recentCompletedCatalogSessions.erase(it);
      return false;
    }
    return true;
  }

  void rememberCompletedSession(const std::string& sessionKey) {
    g_recentCompletedCatalogSessions[sessionKey] = std::chrono::steady_clock::now();
  }

  void cleanupCompletedSessionCache(int maxAgeSeconds = 120) {
    auto now = std::chrono::steady_clock::now();
    for (auto it = g_recentCompletedCatalogSessions.begin();
         it != g_recentCompletedCatalogSessions.end();) {
      auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count();
      if (elapsed > maxAgeSeconds) {
        it = g_recentCompletedCatalogSessions.erase(it);
      } else {
        ++it;
      }
    }
  }
}

/**
 * @brief 解析 Catalog 响应为目录节点列表（支持三种节点类型）
 */
bool parseCatalogNodes(const std::string& body, 
                       std::vector<CatalogNodeInfo>& nodes,
                       int& totalSum) {
  nodes.clear();
  
  // 检查是否是 Catalog 消息
  if (body.find("<CmdType>Notify</CmdType>") == std::string::npos &&
      body.find("<CmdType>Catalog</CmdType>") == std::string::npos) {
    return false;
  }
  
  // 提取总数
  totalSum = extractXmlTagInt(body, "SumNum", 0);
  
  // 提取 DeviceList Num 属性（当前消息包含的节点数量）
  int deviceListNum = 0;
  size_t deviceListPos = body.find("<DeviceList");
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
  
  // 查找所有 DeviceList Item（支持带属性的标签如 <Item Index="X">）
  size_t pos = 0;
  while (true) {
    // 查找 <Item 开头（可能带属性）
    size_t itemStart = body.find("<Item", pos);
    if (itemStart == std::string::npos) break;
    
    // 查找 > 结束标签开始
    size_t tagEnd = body.find(">", itemStart);
    if (tagEnd == std::string::npos) break;
    
    // 查找 </Item> 结束标签
    size_t endPos = body.find("</Item>", tagEnd);
    if (endPos == std::string::npos) break;
    
    // 提取 Item 内容
    std::string itemXml = body.substr(tagEnd + 1, endPos - tagEnd - 1);
    
    // 解析 Item Index 属性（从 <Item Index="X"> 中提取）
    std::string itemTag = body.substr(itemStart, tagEnd - itemStart + 1);
    std::string indexStr = extractXmlAttribute(itemTag, "Index");
    int itemIndex = -1;
    if (!indexStr.empty()) {
      itemIndex = std::atoi(indexStr.c_str());
    }
    
    // 解析节点信息
    CatalogNodeInfo node;
    node.nodeId = extractXmlTag(itemXml, "DeviceID");
    
    // Name 字段从 GB2312 转换为 UTF-8（下级平台使用 GB2312 编码）
    std::string rawName = extractXmlTag(itemXml, "Name");
    node.name = gb2312ToUtf8(rawName);
    
    node.parentId = extractXmlTag(itemXml, "ParentID");
    node.civilCode = extractXmlTag(itemXml, "CivilCode");
    node.parental = extractXmlTagInt(itemXml, "Parental", 0);
    
    // 解析业务分组ID（BusinessGroupID）
    node.businessGroupId = extractXmlTag(itemXml, "BusinessGroupID");
    
    // 设置 item_num（当前 DeviceList 包含的节点数量）
    node.itemNum = deviceListNum;
    
    // 设置 item_index（当前节点在设备列表中的索引）
    node.itemIndex = itemIndex;
    
    // 判断节点类型（支持3位编码和civicode兼容）
    node.nodeType = getNodeTypeFromDeviceId(node.nodeId, node.civilCode);
    
    // 设备特有字段（所有节点都解析，存储备用）
    node.manufacturer = extractXmlTag(itemXml, "Manufacturer");
    node.model = extractXmlTag(itemXml, "Model");
    node.owner = extractXmlTag(itemXml, "Owner");
    node.address = extractXmlTag(itemXml, "Address");
    node.safetyWay = extractXmlTagInt(itemXml, "SafetyWay", 0);
    node.registerWay = extractXmlTagInt(itemXml, "RegisterWay", 1);
    node.secrecy = extractXmlTag(itemXml, "Secrecy");
    if (node.secrecy.empty()) node.secrecy = "0";
    node.status = extractXmlTag(itemXml, "Status");
    
    // 解析经纬度
    std::string lonStr = extractXmlTag(itemXml, "Longitude");
    std::string latStr = extractXmlTag(itemXml, "Latitude");
    if (!lonStr.empty()) node.longitude = std::stod(lonStr);
    if (!latStr.empty()) node.latitude = std::stod(latStr);
    
    // 解析 GB28181-2016 扩展字段（设备安全相关）
    node.block = extractXmlTag(itemXml, "Block");
    node.certNum = extractXmlTag(itemXml, "CertNum");
    
    std::string certifiableStr = extractXmlTag(itemXml, "Certifiable");
    if (!certifiableStr.empty()) {
        node.certifiable = std::atoi(certifiableStr.c_str());
    }
    
    node.errCode = extractXmlTag(itemXml, "ErrCode");
    node.errTime = extractXmlTag(itemXml, "ErrTime");
    node.ipAddress = extractXmlTag(itemXml, "IPAddress");
    
    std::string portStr = extractXmlTag(itemXml, "Port");
    if (!portStr.empty()) {
        node.port = std::atoi(portStr.c_str());
    }
    
    // 只添加有效的节点（有 DeviceID）
    if (!node.nodeId.empty()) {
      nodes.push_back(node);
      InfoL << "【CATALOG NODE】Index=" << itemIndex
            << " Type=" << getNodeTypeName(node.nodeType) 
            << "(" << getDeviceTypeDescription(node.nodeId) << ")"
            << " ID=" << node.nodeId 
            << " Name=" << (node.name.empty() ? "(empty)" : node.name)
            << " ParentID=" << (node.parentId.empty() ? "(empty)" : node.parentId)
            << " BusinessGroupID=" << (node.businessGroupId.empty() ? "(empty)" : node.businessGroupId)
            << " CivilCode=" << (node.civilCode.empty() ? "(empty)" : node.civilCode)
            << " Status=" << (node.status.empty() ? "(empty)" : node.status);
    } else {
      WarnL << "【CATALOG SKIP】Item Index=" << itemIndex << " has no DeviceID, itemTag=" << itemTag;
    }
    
    pos = endPos + 7;
  }
  
  InfoL << "【CATALOG PARSED】Total nodes=" << nodes.size() << " Expected=" << totalSum;
  return !nodes.empty() || totalSum > 0;
}

/**
 * @brief CatalogSession 方法实现
 */
void CatalogSession::addNodes(const std::vector<CatalogNodeInfo>& nodes) {
  std::lock_guard<std::mutex> lock(g_sessionsMutex);
  for (const auto& node : nodes) {
    allNodes.push_back(node);
    receivedCount++;
    DebugL << "【CatalogSession】Adding node: " << node.nodeId 
           << " (" << getNodeTypeName(node.nodeType) << ")";
  }
  lastUpdate = std::chrono::steady_clock::now();
  InfoL << "【CatalogSession】Added " << nodes.size() << " nodes, total=" 
        << receivedCount << "/" << expectedTotal
        << " isComplete=" << (isComplete() ? "YES" : "NO");
}

bool CatalogSession::isTimeout(int seconds) const {
  auto now = std::chrono::steady_clock::now();
  auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastUpdate).count();
  return elapsed > seconds;
}

void CatalogSession::saveToDatabase(int platformId) {
  if (isSaved) return;
  
  InfoL << "【CatalogSession】Saving " << allNodes.size() << " nodes to database";
  
  int deviceCount = 0;
  int dirCount = 0;
  int regionCount = 0;
  
  for (const auto& node : allNodes) {
    // 根据节点类型分别计数
    switch (node.nodeType) {
      case CatalogNodeType::DEVICE: deviceCount++; break;
      case CatalogNodeType::DIRECTORY: dirCount++; break;
      case CatalogNodeType::REGION: regionCount++; break;
    }
    
    // 构建插入/更新 SQL
    std::string nodeTypeStr = std::to_string(static_cast<int>(node.nodeType));
    std::string parentalStr = std::to_string(node.parental);
    std::string safetyWayStr = std::to_string(node.safetyWay);
    std::string registerWayStr = std::to_string(node.registerWay);
    std::string longitudeStr = std::to_string(node.longitude);
    std::string latitudeStr = std::to_string(node.latitude);
    std::string certifiableStr = std::to_string(node.certifiable);
    std::string portStr = std::to_string(node.port);
    std::string itemIndexStr = std::to_string(node.itemIndex);
    
    // 先检查节点是否已存在
    std::string checkSql = "SELECT id FROM catalog_nodes WHERE platform_id = " + 
        std::to_string(platformId) + " AND node_id = '" + 
        gb::escapeSqlString(node.nodeId) + "'";
    std::string existing = execPsql(checkSql.c_str());
    
    if (existing.empty()) {
      // 插入新节点（包含所有 GB28181-2016 字段）
      std::string insertSql =
        "INSERT INTO catalog_nodes (node_id, device_id, platform_id, platform_gb_id, parent_id, name, "
        "node_type, manufacturer, model, owner, civil_code, address, parental, "
        "safety_way, register_way, secrecy, status, longitude, latitude, "
        "block, cert_num, certifiable, err_code, err_time, ip_address, port, "
        "item_num, item_index, business_group_id) "
        "VALUES ('" + gb::escapeSqlString(node.nodeId) + "', '" +
        gb::escapeSqlString(node.nodeId) + "', " +  // device_id 与 node_id 一致
        std::to_string(platformId) + ", '" +
        gb::escapeSqlString(platformGbId) + "', '" +
        gb::escapeSqlString(node.parentId) + "', '" +
        gb::escapeSqlString(node.name.empty() ? node.nodeId : node.name) + "', " +
        nodeTypeStr + ", '" +
        gb::escapeSqlString(node.manufacturer) + "', '" +
        gb::escapeSqlString(node.model) + "', '" +
        gb::escapeSqlString(node.owner) + "', '" +
        gb::escapeSqlString(node.civilCode) + "', '" +
        gb::escapeSqlString(node.address) + "', " +
        parentalStr + ", " +
        safetyWayStr + ", " +
        registerWayStr + ", '" +
        node.secrecy + "', '" +
        gb::escapeSqlString(node.status) + "', " +
        longitudeStr + ", " +
        latitudeStr + ", '" +
        gb::escapeSqlString(node.block) + "', '" +
        gb::escapeSqlString(node.certNum) + "', " +
        certifiableStr + ", '" +
        gb::escapeSqlString(node.errCode) + "', '" +
        gb::escapeSqlString(node.errTime) + "', '" +
        gb::escapeSqlString(node.ipAddress) + "', " +
        portStr + ", " +
        std::to_string(node.itemNum) + ", " +
        itemIndexStr + ", '" +
        gb::escapeSqlString(node.businessGroupId) + "')";
      
      DebugL << "【SQL INSERT】nodeId=" << node.nodeId << " platformId=" << platformId;
      bool insertResult = execPsqlCommand(insertSql);
      if (!insertResult) {
        ErrorL << "Failed to insert catalog node: " << node.nodeId << " SQL: " << insertSql.substr(0, 200);
      } else {
        DebugL << "【SQL INSERT SUCCESS】nodeId=" << node.nodeId;
      }
    } else {
      // 更新现有节点（包含所有 GB28181-2016 字段）
      std::string updateSql =
        "UPDATE catalog_nodes SET "
        "device_id = '" + gb::escapeSqlString(node.nodeId) + "', " +  // device_id 与 node_id 一致
        "name = '" + gb::escapeSqlString(node.name.empty() ? node.nodeId : node.name) + "', " +
        "parent_id = '" + gb::escapeSqlString(node.parentId) + "', " +
        "node_type = " + nodeTypeStr + ", " +
        "manufacturer = '" + gb::escapeSqlString(node.manufacturer) + "', " +
        "model = '" + gb::escapeSqlString(node.model) + "', " +
        "owner = '" + gb::escapeSqlString(node.owner) + "', " +
        "civil_code = '" + gb::escapeSqlString(node.civilCode) + "', " +
        "address = '" + gb::escapeSqlString(node.address) + "', " +
        "parental = " + parentalStr + ", " +
        "safety_way = " + safetyWayStr + ", " +
        "register_way = " + registerWayStr + ", " +
        "secrecy = '" + node.secrecy + "', " +
        "status = '" + gb::escapeSqlString(node.status) + "', " +
        "longitude = " + longitudeStr + ", " +
        "latitude = " + latitudeStr + ", " +
        "block = '" + gb::escapeSqlString(node.block) + "', " +
        "cert_num = '" + gb::escapeSqlString(node.certNum) + "', " +
        "certifiable = " + certifiableStr + ", " +
        "err_code = '" + gb::escapeSqlString(node.errCode) + "', " +
        "err_time = '" + gb::escapeSqlString(node.errTime) + "', " +
        "ip_address = '" + gb::escapeSqlString(node.ipAddress) + "', " +
        "port = " + portStr + ", " +
        "item_num = " + std::to_string(node.itemNum) + ", " +
        "item_index = " + itemIndexStr + ", " +
        "business_group_id = '" + gb::escapeSqlString(node.businessGroupId) + "', " +
        "updated_at = CURRENT_TIMESTAMP " +
        "WHERE platform_id = " + std::to_string(platformId) +
        " AND node_id = '" + gb::escapeSqlString(node.nodeId) + "'";
      
      DebugL << "【SQL UPDATE】nodeId=" << node.nodeId << " platformId=" << platformId;
      bool updateResult = execPsqlCommand(updateSql);
      if (!updateResult) {
        ErrorL << "Failed to update catalog node: " << node.nodeId;
      } else {
        DebugL << "【SQL UPDATE SUCCESS】nodeId=" << node.nodeId;
      }
    }
    
    // 仅可预览摄像头（131/132）写入 cameras 表；
    // 如 181/200 等设备类目录节点仅保留在 catalog_nodes，不进入摄像头管理/预览链路。
    if (node.isCamera()) {
      // 获取刚插入/更新的 catalog_nodes 记录 ID
      std::string nodeRefSql = "SELECT id FROM catalog_nodes WHERE platform_id = " + 
          std::to_string(platformId) + " AND node_id = '" + 
          gb::escapeSqlString(node.nodeId) + "'";
      std::string nodeRefId = execPsql(nodeRefSql.c_str());
      
      // 检查 cameras 表是否已存在该设备
      const std::string escPlatGbN = gb::escapeSqlString(platformGbId);
      const std::string escNodeId = gb::escapeSqlString(node.nodeId);
      std::string checkCameraSql = "SELECT id::text FROM cameras WHERE platform_gb_id = '" + escPlatGbN +
                                   "' AND device_gb_id = '" + escNodeId + "'";
      std::string existingCamera = execPsql(checkCameraSql.c_str());
      if (!existingCamera.empty()) {
        size_t nlc = existingCamera.find('\n');
        if (nlc != std::string::npos) existingCamera = existingCamera.substr(0, nlc);
        existingCamera = trimSqlField(existingCamera);
      }

      std::string onlineStr = (node.status == "ON" || node.status == "OK") ? "true" : "false";

      if (existingCamera.empty()) {
        std::string insertCameraSql =
            "INSERT INTO cameras (device_gb_id, name, platform_id, platform_gb_id, online, "
            "node_id, node_ref, manufacturer, model, owner, civil_code, address, "
            "parental, parent_id, safety_way, register_way, secrecy) "
            "VALUES ('" +
            escNodeId + "', '" + gb::escapeSqlString(node.name.empty() ? node.nodeId : node.name) + "', " +
            std::to_string(platformId) + ", '" + escPlatGbN + "', " + onlineStr + ", '" + escNodeId + "', " +
            (nodeRefId.empty() ? "NULL" : nodeRefId) + ", '" + gb::escapeSqlString(node.manufacturer) + "', '" +
            gb::escapeSqlString(node.model) + "', '" + gb::escapeSqlString(node.owner) + "', '" +
            gb::escapeSqlString(node.civilCode) + "', '" + gb::escapeSqlString(node.address) + "', " +
            std::to_string(node.parental) + ", '" + gb::escapeSqlString(node.parentId) + "', " +
            std::to_string(node.safetyWay) + ", " + std::to_string(node.registerWay) + ", '" + node.secrecy +
            "')";

        bool cameraInsertResult = execPsqlCommand(insertCameraSql);
        if (cameraInsertResult) {
          InfoL << "【CAMERA INSERT SUCCESS】deviceId=" << node.nodeId;
        } else {
          ErrorL << "【CAMERA INSERT FAILED】deviceId=" << node.nodeId;
        }
      } else {
        std::string updateCameraSql = "UPDATE cameras SET "
                                      "name = '" +
                                      gb::escapeSqlString(node.name.empty() ? node.nodeId : node.name) + "', " +
                                      "platform_id = " + std::to_string(platformId) + ", " + "platform_gb_id = '" +
                                      escPlatGbN + "', " + "online = " + onlineStr + ", " + "node_id = '" +
                                      escNodeId + "', " + "node_ref = " + (nodeRefId.empty() ? "NULL" : nodeRefId) +
                                      ", " + "manufacturer = '" + gb::escapeSqlString(node.manufacturer) + "', " +
                                      "model = '" + gb::escapeSqlString(node.model) + "', " + "owner = '" +
                                      gb::escapeSqlString(node.owner) + "', " + "civil_code = '" +
                                      gb::escapeSqlString(node.civilCode) + "', " + "address = '" +
                                      gb::escapeSqlString(node.address) + "', " + "parental = " +
                                      std::to_string(node.parental) + ", " + "parent_id = '" +
                                      gb::escapeSqlString(node.parentId) + "', " + "safety_way = " +
                                      std::to_string(node.safetyWay) + ", " + "register_way = " +
                                      std::to_string(node.registerWay) + ", " + "secrecy = '" + node.secrecy +
                                      "', " + "updated_at = CURRENT_TIMESTAMP " + "WHERE id = " + existingCamera;

        bool cameraUpdateResult = execPsqlCommand(updateCameraSql);
        if (cameraUpdateResult) {
          InfoL << "【CAMERA UPDATE SUCCESS】deviceId=" << node.nodeId;
        } else {
          ErrorL << "【CAMERA UPDATE FAILED】deviceId=" << node.nodeId;
        }
      }
    }
  }
  
  InfoL << "【CatalogSession】Saved: Devices=" << deviceCount 
        << " Directories=" << dirCount << " Regions=" << regionCount;
  
  isSaved = true;
}

/**
 * @brief 处理 Catalog Notify（新接口，支持目录树和分页）
 */
void handleCatalogTreeNotify(const std::string& platformGbId, 
                              const std::string& body, 
                              int sn) {
  std::vector<CatalogNodeInfo> nodes;
  int totalSum = 0;
  
  if (!parseCatalogNodes(body, nodes, totalSum)) {
    g_queryInProgress[platformGbId] = false;
    return;
  }
  
  // 获取目标平台ID（从 XML 中的 DeviceID）
  std::string targetPlatformGbId = extractXmlTag(body, "DeviceID");
  if (targetPlatformGbId.empty()) {
    targetPlatformGbId = platformGbId;
  }
  
  // 查询平台数据库ID
  std::string sql = "SELECT id FROM device_platforms WHERE gb_id = '" +
      gb::escapeSqlString(targetPlatformGbId) + "'";
  std::string idStr = execPsql(sql.c_str());
  int platformId = 0;
  if (!idStr.empty()) {
    platformId = std::atoi(idStr.c_str());
  }
  
  if (platformId <= 0) {
    WarnL << "【CATALOG TREE】Platform not found: " << targetPlatformGbId;
    g_queryInProgress[targetPlatformGbId] = false;
    g_queryInProgress[platformGbId] = false;
    return;
  }
  
  // 获取或创建 session（修复：不要提前移出map，直接在里面操作）
  std::string sessionKey = makeSessionKey(targetPlatformGbId, sn);
  CatalogSession* session = nullptr;
  bool isNewSession = false;
  
  {
    std::lock_guard<std::mutex> lock(g_sessionsMutex);
    cleanupCompletedSessionCache();
    if (isRecentlyCompletedSession(sessionKey)) {
      InfoL << "【CatalogSession】Ignore duplicated completed session: " << sessionKey;
      return;
    }
    auto it = g_catalogSessions.find(sessionKey);
    if (it == g_catalogSessions.end()) {
      // 新 session
      auto newSession = std::make_unique<CatalogSession>(targetPlatformGbId, sn, totalSum);
      session = newSession.get();
      g_catalogSessions[sessionKey] = std::move(newSession);
      isNewSession = true;
      InfoL << "【CatalogSession】Created new session: " << sessionKey 
            << " expectedTotal=" << totalSum;
    } else {
      session = it->second.get();
      InfoL << "【CatalogSession】Found existing session: " << sessionKey
            << " current=" << session->receivedCount << "/" << session->expectedTotal;
    }
  }
  
  // 添加节点到 session
  session->addNodes(nodes);
  
  // 检查是否完成或超时
  bool shouldSave = session->isComplete() || session->isTimeout(30);
  
  if (shouldSave) {
    // 保存到数据库
    session->saveToDatabase(platformId);
    
    // 调用回调通知前端
    if (g_catalogCallback) {
      // 将节点转换为 CameraInfo 列表（保持兼容性）
      std::vector<CameraInfo> cameras;
      for (const auto& node : session->allNodes) {
        if (node.isCamera()) {
          CameraInfo cam;
          cam.deviceId = node.nodeId;
          cam.name = node.name;
          cam.manufacturer = node.manufacturer;
          cam.model = node.model;
          cam.status = node.status;
          cameras.push_back(cam);
        }
      }
      g_catalogCallback(targetPlatformGbId, cameras, totalSum);
    }
    
    // 保存完成后从 map 中移除
    {
      std::lock_guard<std::mutex> lock(g_sessionsMutex);
      rememberCompletedSession(sessionKey);
      g_catalogSessions.erase(sessionKey);
      InfoL << "【CatalogSession】Session completed and removed: " << sessionKey;
    }
    g_queryInProgress[targetPlatformGbId] = false;
    if (platformGbId != targetPlatformGbId) {
      g_queryInProgress[platformGbId] = false;
    }
  } else {
    InfoL << "【CatalogSession】Waiting for more data: " << sessionKey
          << " progress=" << session->receivedCount << "/" << session->expectedTotal;
  }
}

/**
 * @brief 清理过期的 Catalog Session
 */
void cleanupCatalogSessions(int maxAgeSeconds) {
  std::lock_guard<std::mutex> lock(g_sessionsMutex);
  auto now = std::chrono::steady_clock::now();
  cleanupCompletedSessionCache(maxAgeSeconds * 2);
  
  for (auto it = g_catalogSessions.begin(); it != g_catalogSessions.end();) {
    if (it->second->isTimeout(maxAgeSeconds)) {
      InfoL << "【CatalogSession】Cleaning up expired session: " << it->first;
      if (it->second) {
        g_queryInProgress[it->second->platformGbId] = false;
      }
      it = g_catalogSessions.erase(it);
    } else {
      ++it;
    }
  }
}

/**
 * @brief 保存目录节点到数据库（兼容旧接口，保存完整字段）
 */
int saveCatalogNodesToDb(int platformId, const std::string& platformGbId, 
                          const std::vector<CatalogNodeInfo>& nodes) {
  int savedCount = 0;
  
  for (const auto& node : nodes) {
    // 根据节点类型分别保存
    std::string nodeTypeStr = std::to_string(static_cast<int>(node.nodeType));
    std::string deviceName = node.name.empty() ? ("Node_" + node.nodeId) : node.name;
    std::string parentalStr = std::to_string(node.parental);
    std::string safetyWayStr = std::to_string(node.safetyWay);
    std::string registerWayStr = std::to_string(node.registerWay);
    std::string longitudeStr = std::to_string(node.longitude);
    std::string latitudeStr = std::to_string(node.latitude);
    std::string certifiableStr = std::to_string(node.certifiable);
    std::string portStr = std::to_string(node.port);
    std::string itemIndexStr = std::to_string(node.itemIndex);
    
    // 检查节点是否已存在
    std::string checkSql = "SELECT id FROM catalog_nodes WHERE platform_id = " + 
        std::to_string(platformId) + " AND node_id = '" + 
        gb::escapeSqlString(node.nodeId) + "'";
    std::string existing = execPsql(checkSql.c_str());
    
    if (existing.empty()) {
      // 插入新节点（包含完整字段）
      std::string insertSql =
        "INSERT INTO catalog_nodes (node_id, device_id, platform_id, platform_gb_id, parent_id, name, "
        "node_type, manufacturer, model, owner, civil_code, address, parental, "
        "safety_way, register_way, secrecy, status, longitude, latitude, "
        "block, cert_num, certifiable, err_code, err_time, ip_address, port, "
        "item_num, item_index, business_group_id) "
        "VALUES ('" + gb::escapeSqlString(node.nodeId) + "', '" +
        gb::escapeSqlString(node.nodeId) + "', " +  // device_id 与 node_id 一致
        std::to_string(platformId) + ", '" +
        gb::escapeSqlString(platformGbId) + "', '" +
        gb::escapeSqlString(node.parentId) + "', '" +
        gb::escapeSqlString(deviceName) + "', " +
        nodeTypeStr + ", '" +
        gb::escapeSqlString(node.manufacturer) + "', '" +
        gb::escapeSqlString(node.model) + "', '" +
        gb::escapeSqlString(node.owner) + "', '" +
        gb::escapeSqlString(node.civilCode) + "', '" +
        gb::escapeSqlString(node.address) + "', " +
        parentalStr + ", " +
        safetyWayStr + ", " +
        registerWayStr + ", '" +
        node.secrecy + "', '" +
        gb::escapeSqlString(node.status) + "', " +
        longitudeStr + ", " +
        latitudeStr + ", '" +
        gb::escapeSqlString(node.block) + "', '" +
        gb::escapeSqlString(node.certNum) + "', " +
        certifiableStr + ", '" +
        gb::escapeSqlString(node.errCode) + "', '" +
        gb::escapeSqlString(node.errTime) + "', '" +
        gb::escapeSqlString(node.ipAddress) + "', " +
        portStr + ", " +
        std::to_string(node.itemNum) + ", " +
        itemIndexStr + ", '" +
        gb::escapeSqlString(node.businessGroupId) + "')";

      if (execPsqlCommand(insertSql)) {
        savedCount++;
      }
    } else {
      // 更新现有节点（包含完整字段）
      std::string updateSql =
        "UPDATE catalog_nodes SET "
        "device_id = '" + gb::escapeSqlString(node.nodeId) + "', " +  // device_id 与 node_id 一致
        "name = '" + gb::escapeSqlString(deviceName) + "', " +
        "parent_id = '" + gb::escapeSqlString(node.parentId) + "', " +
        "node_type = " + nodeTypeStr + ", " +
        "manufacturer = '" + gb::escapeSqlString(node.manufacturer) + "', " +
        "model = '" + gb::escapeSqlString(node.model) + "', " +
        "owner = '" + gb::escapeSqlString(node.owner) + "', " +
        "civil_code = '" + gb::escapeSqlString(node.civilCode) + "', " +
        "address = '" + gb::escapeSqlString(node.address) + "', " +
        "parental = " + parentalStr + ", " +
        "safety_way = " + safetyWayStr + ", " +
        "register_way = " + registerWayStr + ", " +
        "secrecy = '" + node.secrecy + "', " +
        "status = '" + gb::escapeSqlString(node.status) + "', " +
        "longitude = " + longitudeStr + ", " +
        "latitude = " + latitudeStr + ", " +
        "block = '" + gb::escapeSqlString(node.block) + "', " +
        "cert_num = '" + gb::escapeSqlString(node.certNum) + "', " +
        "certifiable = " + certifiableStr + ", " +
        "err_code = '" + gb::escapeSqlString(node.errCode) + "', " +
        "err_time = '" + gb::escapeSqlString(node.errTime) + "', " +
        "ip_address = '" + gb::escapeSqlString(node.ipAddress) + "', " +
        "port = " + portStr + ", " +
        "item_num = " + std::to_string(node.itemNum) + ", " +
        "item_index = " + itemIndexStr + ", " +
        "business_group_id = '" + gb::escapeSqlString(node.businessGroupId) + "', " +
        "updated_at = CURRENT_TIMESTAMP " +
        "WHERE platform_id = " + std::to_string(platformId) + 
        " AND node_id = '" + gb::escapeSqlString(node.nodeId) + "'";
      
      if (execPsqlCommand(updateSql)) {
        savedCount++;
      }
    }
  }
  
  InfoL << "【saveCatalogNodesToDb】Saved/Updated " << savedCount << " nodes";
  return savedCount;
}

// ========== 视频预览（点播）功能实现 ==========

/**
 * @brief 获取设备IP地址
 * @details 从device_platforms表查询设备的Contact IP地址
 */
std::string getDeviceIp(const std::string& deviceGbId) {
  std::string sql =
      "SELECT COALESCE(NULLIF(TRIM(signal_src_ip), ''), NULLIF(TRIM(contact_ip), '')) "
      "FROM device_platforms WHERE gb_id = '" +
      escapeSqlString(deviceGbId) + "'";
  std::string out = execPsql(sql.c_str());
  
  if (out.empty()) {
    return "";
  }
  
  // 去除换行和空白
  size_t end = out.find('\n');
  if (end != std::string::npos) {
    out = out.substr(0, end);
  }
  
  size_t start = out.find_first_not_of(" \t\r\n");
  if (start != std::string::npos) {
    out = out.substr(start);
  }
  
  size_t last = out.find_last_not_of(" \t\r\n");
  if (last != std::string::npos) {
    out = out.substr(0, last + 1);
  }
  
  return out;
}

/**
 * @brief 发送点播INVITE请求
 * @details 构造SDP并发送INVITE到设备，使用PJSIP客户端事务
 *          符合GB28181-2016 第9章 视频回放控制要求
 */
bool sendPlayInvite(const std::string& deviceGbId,
                    const std::string& channelId,
                    uint16_t zlmPort,
                    std::string& outCallId,
                    std::string& outSdp,
                    const std::string& sdpConnectionIp,
                    bool rtpOverTcp,
                    uint64_t playbackStartUnix,
                    uint64_t playbackEndUnix,
                    bool isDownload) {
  InfoL << "【sendPlayInvite】Inviting device " << deviceGbId
        << " channel " << channelId << " to port " << zlmPort
        << " rtpOverTcp=" << (rtpOverTcp ? "yes" : "no")
        << " playback=" << (playbackStartUnix > 0 ? "yes" : "no")
        << " download=" << (isDownload ? "yes" : "no");

  if (!g_sip_endpt) {
    ErrorL << "【sendPlayInvite】SIP endpoint not initialized";
    return false;
  }

  std::string contactHost, signalHost;
  int contactPort = 5060, signalPort = 0;
  if (!loadPlatformSipRouting(deviceGbId, contactHost, contactPort, signalHost, signalPort)) {
    WarnL << "【sendPlayInvite】平台未入库: " << deviceGbId;
    return false;
  }
  std::string uriHost;
  int uriPort = 5060;
  if (!resolveUriHostForDownlink(deviceGbId, contactHost, contactPort, signalHost, signalPort,
                                 uriHost, uriPort, "【sendPlayInvite】")) {
    WarnL << "【sendPlayInvite】无可用信令地址: " << deviceGbId;
    return false;
  }
  const std::string sipContactLabel =
      contactUsableAsUriHost(contactHost)
          ? (contactHost + ":" + std::to_string(contactPort > 0 ? contactPort : 5060))
          : (std::string("degraded ") + uriHost + ":" + std::to_string(uriPort));

  // 获取本机配置（IP和端口）
  std::string localHost = "127.0.0.1";
  int localPort = 5060;
  std::string localGbId = "34020000002000000001";

  std::string sql = "SELECT COALESCE(gb_id, '34020000002000000001') FROM gb_local_config LIMIT 1";
  std::string out = execPsql(sql.c_str());
  if (!out.empty()) {
    size_t nl = out.find('\n');
    if (nl != std::string::npos) out = out.substr(0, nl);
    size_t first = out.find_first_not_of(" \t\r\n");
    if (first != std::string::npos) localGbId = out.substr(first);
  }

  sql = "SELECT COALESCE(signal_ip, '127.0.0.1'), COALESCE(signal_port, 5060) FROM gb_local_config LIMIT 1";
  out = execPsql(sql.c_str());
  if (!out.empty()) {
    size_t pipePos = out.find('|');
    if (pipePos != std::string::npos) {
      localHost = out.substr(0, pipePos);
      size_t last = localHost.find_last_not_of(" \t\r\n");
      if (last != std::string::npos) localHost = localHost.substr(0, last + 1);
      std::string portStr = out.substr(pipePos + 1);
      size_t first = portStr.find_first_not_of(" \t\r\n");
      if (first != std::string::npos) portStr = portStr.substr(first);
      if (!portStr.empty()) localPort = std::atoi(portStr.c_str());
    } else {
      localHost = out;
      size_t last = localHost.find_last_not_of(" \t\r\n");
      if (last != std::string::npos) localHost = localHost.substr(0, last + 1);
    }
  }
  /* COALESCE(signal_ip,'127.0.0.1') 仅替换 NULL；库中 signal_ip='' 时仍会得到空串，PJSIP 打印 From/Contact 会 assert(host.slen!=0) */
  if (localHost.empty()) {
    localHost = "127.0.0.1";
    WarnL << "【sendPlayInvite】gb_local_config.signal_ip 为空或仅空白，已回退为 " << localHost;
  }

  // Call-ID：由 sendPlayInviteAsync 入队前已写入 outCallId，须与 HTTP 会话一致；仅在为空时本地生成（兼容直接调用）
  if (outCallId.empty()) {
    static int inviteCounter = 0;
    inviteCounter++;
    outCallId = "play-" + deviceGbId + "-" + channelId + "-" + std::to_string(inviteCounter) + "@" + localHost;
  }

  std::string sdpConn = sdpConnectionIp.empty() ? localHost : sdpConnectionIp;

  InfoL << "【sendPlayInvite】Local: " << localGbId << "@" << localHost << ":" << localPort
        << " SDP连接地址(IN IP4)=" << sdpConn
        << " -> R-URI/To host: " << uriHost << ":" << uriPort
        << " (Contact侧=" << sipContactLabel << ")"
        << " UDP目的(signal)=" << (signalHost.empty() ? "(按URI)" : signalHost + ":" + std::to_string(signalPort))
        << " Call-ID: " << outCallId;

  // 创建内存池
  pj_pool_t* pool = pjsip_endpt_create_pool(g_sip_endpt, "invite_play", 4096, 2048);
  if (!pool) {
    ErrorL << "【sendPlayInvite】Failed to create pool";
    return false;
  }

  // Request-URI：平台国标 ID + Contact（或退化后的）host:port
  std::string targetUriStr = "sip:" + deviceGbId + "@" + uriHost + ":" + std::to_string(uriPort);
  pj_str_t targetUriPj = pj_str((char*)targetUriStr.c_str());
  pjsip_uri* target_uri = pjsip_parse_uri(pool, targetUriPj.ptr, targetUriPj.slen, PJSIP_PARSE_URI_AS_NAMEADDR);
  if (!target_uri) {
    ErrorL << "【sendPlayInvite】Failed to parse target URI: " << targetUriStr;
    pjsip_endpt_release_pool(g_sip_endpt, pool);
    return false;
  }

  // 构建本地URI（From头）
  std::string localUriStr = "sip:" + localGbId + "@" + localHost + ":" + std::to_string(localPort);

  // 创建From头
  pjsip_from_hdr* from_hdr = pjsip_from_hdr_create(pool);
  if (from_hdr) {
    pjsip_name_addr* from_name_addr = pjsip_name_addr_create(pool);
    if (from_name_addr) {
      pjsip_sip_uri* from_uri = pjsip_sip_uri_create(pool, PJ_FALSE);
      if (from_uri) {
        pj_strdup2(pool, &from_uri->user, (char*)localGbId.c_str());
        pj_strdup2(pool, &from_uri->host, (char*)localHost.c_str());
        from_uri->port = localPort;
        from_name_addr->uri = (pjsip_uri*)from_uri;
      }
      from_hdr->uri = (pjsip_uri*)from_name_addr;
    }
  }

  // 创建To头（目标设备）- 使用摄像头ID(channelId)
  pjsip_to_hdr* to_hdr = pjsip_to_hdr_create(pool);
  if (to_hdr) {
    pjsip_name_addr* to_name_addr = pjsip_name_addr_create(pool);
    if (to_name_addr) {
      pjsip_sip_uri* to_uri = pjsip_sip_uri_create(pool, PJ_FALSE);
      if (to_uri) {
        pj_strdup2(pool, &to_uri->user, (char*)channelId.c_str());
        pj_strdup2(pool, &to_uri->host, (char*)uriHost.c_str());
        to_uri->port = uriPort;
        to_name_addr->uri = (pjsip_uri*)to_uri;
      }
      to_hdr->uri = (pjsip_uri*)to_name_addr;
    }
  }

  // 创建请求
  pjsip_method method;
  pjsip_method_set(&method, PJSIP_INVITE_METHOD);

  pjsip_tx_data* tdata = nullptr;
  pj_status_t st = pjsip_endpt_create_request_from_hdr(g_sip_endpt,
                                                        &method,
                                                        target_uri,
                                                        from_hdr,
                                                        to_hdr,
                                                        nullptr,  // Contact头
                                                        nullptr,  // Call-ID头（让PJSIP生成）
                                                        -1,       // CSeq（让PJSIP生成）
                                                        nullptr,  // body
                                                        &tdata);
  if (st != PJ_SUCCESS || !tdata) {
    ErrorL << "【sendPlayInvite】Failed to create request: " << st;
    pjsip_endpt_release_pool(g_sip_endpt, pool);
    return false;
  }

  /* create_request_from_hdr 已将 From/To/Request-URI 克隆到 tdata->pool；临时 pool 仅保留解析树，可立即释放。
   * 后续所有头与 body 必须分配在 tdata->pool 上，否则提前 release_pool 会与 stateless 异步发送竞态导致崩溃。 */
  pjsip_endpt_release_pool(g_sip_endpt, pool);
  pool = nullptr;

  presetTxDataUdpDestFromSignal(tdata, signalHost, signalPort, "【sendPlayInvite】", sipContactLabel);

  pj_pool_t* msg_pool = tdata->pool;

  // 手动设置Call-ID
  pjsip_cid_hdr* call_id_hdr = (pjsip_cid_hdr*)pjsip_msg_find_hdr(tdata->msg, PJSIP_H_CALL_ID, nullptr);
  if (call_id_hdr) {
    pj_strdup2(msg_pool, &call_id_hdr->id, outCallId.c_str());
  }

  // 修正Via头
  pjsip_via_hdr* via = (pjsip_via_hdr*)pjsip_msg_find_hdr(tdata->msg, PJSIP_H_VIA, nullptr);
  if (via) {
    pj_strdup2(msg_pool, &via->transport, (char*)"UDP");
    pj_strdup2(msg_pool, &via->sent_by.host, (char*)localHost.c_str());
    via->sent_by.port = localPort;
    char branch_buf[64];
    static int playInviteBranchSeq = 0;
    playInviteBranchSeq++;
    snprintf(branch_buf, sizeof(branch_buf), "z9hG4bKplay%d", playInviteBranchSeq);
    pj_strdup2(msg_pool, &via->branch_param, branch_buf);
  }

  // 生成SSRC（32位整数，10位十进制数，GB28181用于标识媒体流）
  // 使用随机数生成，范围 1000000000-4294967295（确保10位数字）
  static std::random_device rd;
  static std::mt19937 gen(rd());
  static std::uniform_int_distribution<uint32_t> dis(1000000000, 4294967295);
  uint32_t ssrc = dis(gen);
  std::string ssrcStr = std::to_string(ssrc);
  InfoL << "【sendPlayInvite】Generated SSRC: " << ssrcStr << " for channel " << channelId;

  // 构造SDP（符合GB28181-2016 Annex F.1；TCP 媒体见附录 TCP 传输约定）
  std::ostringstream sdp;
  sdp << "v=0\r\n";
  // o=字段使用摄像头ID；连接地址可与信令 Contact 分离（平台独立流媒体地址 / 系统 media_http_host）
  sdp << "o=" << channelId << " 0 0 IN IP4 " << sdpConn << "\r\n";
  const bool isPlayback =
      playbackStartUnix > 0 && playbackEndUnix >= playbackStartUnix;
  if (isPlayback) {
    // 回放/下载 t= 填 Unix 时间戳（秒），与多数下级平台/其它上级（如 LiveGBS）抓包一致。
    // GB28181-2016: s=Download 模式设备应不受实时约束、以最快速率发送录像数据。
    sdp << "s=" << (isDownload ? "Download" : "Playback") << "\r\n";
    sdp << "c=IN IP4 " << sdpConn << "\r\n";
    sdp << "t=" << playbackStartUnix << " " << playbackEndUnix << "\r\n";
  } else {
    sdp << "s=Play\r\n";
    sdp << "c=IN IP4 " << sdpConn << "\r\n";
    sdp << "t=0 0\r\n";
  }
  if (rtpOverTcp) {
    sdp << "m=video " << zlmPort << " TCP/RTP/AVP 96\r\n";
    sdp << "a=setup:passive\r\n";
    sdp << "a=connection:new\r\n";
  } else {
    sdp << "m=video " << zlmPort << " RTP/AVP 96\r\n";
  }
  sdp << "a=rtpmap:96 PS/90000\r\n";
  sdp << "a=recvonly\r\n";
  // SSRC字段，GB28181用于标识媒体流
  sdp << "y=" << ssrcStr << "\r\n";

  std::string sdpBody = sdp.str();
  InfoL << "【sendPlayInvite】SDP:\n" << sdpBody;

  // 设置Content-Type和body
  pj_str_t type_str = pj_str((char*)"application");
  pj_str_t subtype_str = pj_str((char*)"sdp");
  pj_str_t body_str = pj_str((char*)sdpBody.c_str());
  tdata->msg->body = pjsip_msg_body_create(msg_pool, &type_str, &subtype_str, &body_str);

  // 添加Contact头（RFC 3261要求，设备需要用这个地址回复ACK）
  std::string contactUri = "<sip:" + localGbId + "@" + localHost + ":" + std::to_string(localPort) + ">";
  pj_str_t contact_str = pj_str((char*)contactUri.c_str());
  pjsip_contact_hdr* contact_hdr = pjsip_contact_hdr_create(msg_pool);
  if (contact_hdr) {
    contact_hdr->uri = pjsip_parse_uri(msg_pool, contact_str.ptr, contact_str.slen, PJSIP_PARSE_URI_AS_NAMEADDR);
    if (contact_hdr->uri) {
      pjsip_msg_add_hdr(tdata->msg, (pjsip_hdr*)contact_hdr);
      InfoL << "【sendPlayInvite】Contact header added: " << contactUri;
    }
  }

  // 添加Subject头（GB28181要求；回放常用子码流标识 1）
  std::string subject =
      isPlayback ? (channelId + ":1," + localGbId + ":1") : (channelId + ":0," + localGbId + ":0");
  pj_str_t subj_name = pj_str((char*)"Subject");
  pj_str_t subj_val = pj_str((char*)subject.c_str());
  pjsip_generic_string_hdr* subj_hdr = pjsip_generic_string_hdr_create(msg_pool, &subj_name, &subj_val);
  if (subj_hdr) {
    pjsip_msg_add_hdr(tdata->msg, (pjsip_hdr*)subj_hdr);
  }

  // 打印完整SIP消息用于调试
  char msg_buf[4096];
  pj_ssize_t msg_len = pjsip_msg_print(tdata->msg, msg_buf, sizeof(msg_buf));
  if (msg_len > 0) {
    InfoL << "【sendPlayInvite】SIP INVITE消息:\n" << std::string(msg_buf, msg_len);
  }

  // 发送INVITE请求（使用stateless方式，简化处理）
  st = pjsip_endpt_send_request_stateless(g_sip_endpt, tdata, nullptr, nullptr);
  if (st != PJ_SUCCESS) {
    ErrorL << "【sendPlayInvite】Failed to send INVITE: " << st;
    pjsip_tx_data_dec_ref(tdata);
    return false;
  }

  InfoL << "【sendPlayInvite】INVITE sent successfully, Call-ID: " << outCallId;

  upstreamBridgeRecordDeviceInviteSsrc(outCallId, ssrcStr);

  // 保存SDP用于后续处理
  outSdp = sdpBody;

  /* tdata 由 stateless 发送路径持有并在完成后递减引用；勿再 release 已销毁的 invite_play pool。 */
  return true;
}

/**
 * @brief 发送BYE请求停止点播
 * @details 使用PJSIP发送BYE请求结束视频会话
 * @param callId SIP通话标识
 * @param deviceGbId 设备国标ID（用于获取设备IP）
 * @param channelId 通道ID（摄像头ID，用于构建To头和Request URI）
 */
bool sendPlayBye(const std::string& callId,
                 const std::string& deviceGbId,
                 const std::string& channelId) {
  InfoL << "【sendPlayBye】Sending BYE to device " << deviceGbId
        << " channel " << channelId
        << " callId " << callId;

  if (!g_sip_endpt) {
    ErrorL << "【sendPlayBye】SIP endpoint not initialized";
    return false;
  }

  std::string contactHost, signalHost;
  int contactPort = 5060, signalPort = 0;
  if (!loadPlatformSipRouting(deviceGbId, contactHost, contactPort, signalHost, signalPort)) {
    WarnL << "【sendPlayBye】平台未入库: " << deviceGbId;
    return false;
  }
  std::string uriHost;
  int uriPort = 5060;
  if (!resolveUriHostForDownlink(deviceGbId, contactHost, contactPort, signalHost, signalPort,
                                 uriHost, uriPort, "【sendPlayBye】")) {
    WarnL << "【sendPlayBye】无可用信令地址: " << deviceGbId;
    return false;
  }
  const std::string sipContactLabel =
      contactUsableAsUriHost(contactHost)
          ? (contactHost + ":" + std::to_string(contactPort > 0 ? contactPort : 5060))
          : (std::string("degraded ") + uriHost + ":" + std::to_string(uriPort));

  // 获取本机配置
  std::string localHost = "127.0.0.1";
  int localPort = 5060;
  std::string localGbId = "34020000002000000001";

  std::string sql = "SELECT COALESCE(gb_id, '34020000002000000001') FROM gb_local_config LIMIT 1";
  std::string out = execPsql(sql.c_str());
  if (!out.empty()) {
    size_t nl = out.find('\n');
    if (nl != std::string::npos) out = out.substr(0, nl);
    size_t first = out.find_first_not_of(" \t\r\n");
    if (first != std::string::npos) localGbId = out.substr(first);
  }

  sql = "SELECT COALESCE(signal_ip, '127.0.0.1'), COALESCE(signal_port, 5060) FROM gb_local_config LIMIT 1";
  out = execPsql(sql.c_str());
  if (!out.empty()) {
    size_t pipePos = out.find('|');
    if (pipePos != std::string::npos) {
      localHost = out.substr(0, pipePos);
      size_t last = localHost.find_last_not_of(" \t\r\n");
      if (last != std::string::npos) localHost = localHost.substr(0, last + 1);
      std::string portStr = out.substr(pipePos + 1);
      size_t pf = portStr.find_first_not_of(" \t\r\n");
      if (pf != std::string::npos) portStr = portStr.substr(pf);
      if (!portStr.empty()) localPort = std::atoi(portStr.c_str());
    } else {
      localHost = out;
      size_t last = localHost.find_last_not_of(" \t\r\n");
      if (last != std::string::npos) localHost = localHost.substr(0, last + 1);
    }
  }
  if (localHost.empty()) {
    localHost = "127.0.0.1";
    WarnL << "【sendPlayBye】gb_local_config.signal_ip 为空或仅空白，已回退为 " << localHost;
  }

  // 创建内存池
  pj_pool_t* pool = pjsip_endpt_create_pool(g_sip_endpt, "bye_play", 2048, 1024);
  if (!pool) {
    ErrorL << "【sendPlayBye】Failed to create pool";
    return false;
  }

  std::string targetUriStr = "sip:" + deviceGbId + "@" + uriHost + ":" + std::to_string(uriPort);
  pj_str_t targetUriPj = pj_str((char*)targetUriStr.c_str());
  pjsip_uri* target_uri = pjsip_parse_uri(pool, targetUriPj.ptr, targetUriPj.slen, PJSIP_PARSE_URI_AS_NAMEADDR);
  if (!target_uri) {
    ErrorL << "【sendPlayBye】Failed to parse target URI: " << targetUriStr;
    pjsip_endpt_release_pool(g_sip_endpt, pool);
    return false;
  }

  // 创建From头 - 与INVITE保持一致
  pjsip_from_hdr* from_hdr = pjsip_from_hdr_create(pool);
  if (from_hdr) {
    pjsip_name_addr* from_name_addr = pjsip_name_addr_create(pool);
    if (from_name_addr) {
      pjsip_sip_uri* from_uri = pjsip_sip_uri_create(pool, PJ_FALSE);
      if (from_uri) {
        pj_strdup2(pool, &from_uri->user, (char*)localGbId.c_str());
        pj_strdup2(pool, &from_uri->host, (char*)localHost.c_str());
        from_uri->port = localPort;
        from_name_addr->uri = (pjsip_uri*)from_uri;
      }
      from_hdr->uri = (pjsip_uri*)from_name_addr;
    }
  }

  // 创建To头 - 使用channelId（摄像头ID）
  pjsip_to_hdr* to_hdr = pjsip_to_hdr_create(pool);
  if (to_hdr) {
    pjsip_name_addr* to_name_addr = pjsip_name_addr_create(pool);
    if (to_name_addr) {
      pjsip_sip_uri* to_uri = pjsip_sip_uri_create(pool, PJ_FALSE);
      if (to_uri) {
        pj_strdup2(pool, &to_uri->user, (char*)channelId.c_str());
        pj_strdup2(pool, &to_uri->host, (char*)uriHost.c_str());
        to_uri->port = uriPort;
        to_name_addr->uri = (pjsip_uri*)to_uri;
      }
      to_hdr->uri = (pjsip_uri*)to_name_addr;
    }
  }

  // 创建BYE请求
  pjsip_method method;
  pjsip_method_set(&method, PJSIP_BYE_METHOD);

  pjsip_tx_data* tdata = nullptr;
  pj_status_t st = pjsip_endpt_create_request_from_hdr(g_sip_endpt,
                                                        &method,
                                                        target_uri,
                                                        from_hdr,
                                                        to_hdr,
                                                        nullptr,
                                                        nullptr,
                                                        -1,
                                                        nullptr,
                                                        &tdata);
  if (st != PJ_SUCCESS || !tdata) {
    ErrorL << "【sendPlayBye】Failed to create request: " << st;
    pjsip_endpt_release_pool(g_sip_endpt, pool);
    return false;
  }

  presetTxDataUdpDestFromSignal(tdata, signalHost, signalPort, "【sendPlayBye】", sipContactLabel);

  // 设置Call-ID
  pjsip_cid_hdr* call_id_hdr = (pjsip_cid_hdr*)pjsip_msg_find_hdr(tdata->msg, PJSIP_H_CALL_ID, nullptr);
  if (call_id_hdr) {
    pj_strdup2(pool, &call_id_hdr->id, callId.c_str());
  }

  // 修正Via头
  static int byeCounter = 0;
  byeCounter++;
  pjsip_via_hdr* via = (pjsip_via_hdr*)pjsip_msg_find_hdr(tdata->msg, PJSIP_H_VIA, nullptr);
  if (via) {
    pj_strdup2(pool, &via->transport, (char*)"UDP");
    pj_strdup2(pool, &via->sent_by.host, (char*)localHost.c_str());
    via->sent_by.port = localPort;
    char branch_buf[64];
    snprintf(branch_buf, sizeof(branch_buf), "z9hG4bKbye%d", byeCounter);
    pj_strdup2(pool, &via->branch_param, branch_buf);
  }

  // 打印SIP消息
  char msg_buf[2048];
  pj_ssize_t msg_len = pjsip_msg_print(tdata->msg, msg_buf, sizeof(msg_buf));
  if (msg_len > 0) {
    InfoL << "【sendPlayBye】SIP BYE消息:\n" << std::string(msg_buf, msg_len);
  }

  // 发送BYE请求
  st = pjsip_endpt_send_request_stateless(g_sip_endpt, tdata, nullptr, nullptr);
  if (st != PJ_SUCCESS) {
    ErrorL << "【sendPlayBye】Failed to send BYE: " << st;
    pjsip_endpt_release_pool(g_sip_endpt, pool);
    return false;
  }

  InfoL << "【sendPlayBye】BYE sent successfully for call " << callId;
  pjsip_endpt_release_pool(g_sip_endpt, pool);
  return true;
}

/**
 * @brief 处理INVITE响应
 */
void handleInviteResponse(const std::string& callId,
                            const std::string& sdpBody,
                            bool isSuccess) {
  if (isSuccess) {
    InfoL << "【handleInviteResponse】INVITE success for call " << callId;
    InfoL << "【handleInviteResponse】Device SDP:\n" << sdpBody;

    // 可以在这里解析设备的SDP，提取设备的发送端口等信息
    // 更新会话状态为STREAMING
  } else {
    WarnL << "【handleInviteResponse】INVITE failed for call " << callId;

    // 清理会话状态
  }
}

/**
 * @brief 异步发送点播INVITE请求
 * @details 将INVITE请求加入队列，由PJSIP工作线程实际发送
 *          适用于从非PJSIP线程调用（如HTTP线程）
 */
bool sendPlayInviteAsync(const std::string& deviceGbId,
                         const std::string& channelId,
                         uint16_t zlmPort,
                         std::string& outCallId,
                         const std::string& sdpConnectionIp,
                         bool rtpOverTcp,
                         uint64_t playbackStartUnix,
                         uint64_t playbackEndUnix,
                         bool isDownload) {
  std::lock_guard<std::mutex> lock(g_invitesMutex);

  // 生成Call-ID（使用计数器确保唯一性）
  g_inviteCounter++;

  // 获取本机IP用于Call-ID
  std::string localHost = "127.0.0.1";
  std::string sql = "SELECT COALESCE(signal_ip, '127.0.0.1') FROM gb_local_config LIMIT 1";
  std::string out = execPsql(sql.c_str());
  if (!out.empty()) {
    size_t nl = out.find('\n');
    if (nl != std::string::npos) out = out.substr(0, nl);
    size_t first = out.find_first_not_of(" \t\r\n");
    if (first != std::string::npos) localHost = out.substr(first);
  }
  if (localHost.empty()) localHost = "127.0.0.1";

  outCallId = "play-" + deviceGbId + "-" + channelId + "-" +
              std::to_string(g_inviteCounter) + "@" + localHost;

  // 添加到队列
  PendingInvite invite;
  invite.deviceGbId = deviceGbId;
  invite.channelId = channelId;
  invite.zlmPort = zlmPort;
  invite.callId = outCallId;
  invite.sdpConnectionIp = sdpConnectionIp;
  invite.rtpOverTcp = rtpOverTcp;
  invite.playbackStartUnix = playbackStartUnix;
  invite.playbackEndUnix = playbackEndUnix;
  invite.isDownload = isDownload;
  g_pendingInvites.push_back(invite);

  InfoL << "【sendPlayInviteAsync】Invite queued for device " << deviceGbId
        << " channel " << channelId << " Call-ID: " << outCallId;
  return true;
}

bool enqueuePlayInviteWithCallId(const std::string& platformGbId,
                                 const std::string& channelId,
                                 uint16_t zlmPort,
                                 const std::string& callId,
                                 const std::string& sdpConnectionIp,
                                 bool rtpOverTcp,
                                 uint64_t playbackStartUnix,
                                 uint64_t playbackEndUnix,
                                 bool isDownload) {
  if (callId.empty()) return false;
  std::lock_guard<std::mutex> lock(g_invitesMutex);
  PendingInvite invite;
  invite.deviceGbId = platformGbId;
  invite.channelId = channelId;
  invite.zlmPort = zlmPort;
  invite.callId = callId;
  invite.sdpConnectionIp = sdpConnectionIp;
  invite.rtpOverTcp = rtpOverTcp;
  invite.playbackStartUnix = playbackStartUnix;
  invite.playbackEndUnix = playbackEndUnix;
  invite.isDownload = isDownload;
  g_pendingInvites.push_back(invite);
  InfoL << "【enqueuePlayInviteWithCallId】queued platform=" << platformGbId << " channel=" << channelId
        << " Call-ID=" << callId;
  return true;
}

/**
 * @brief 处理待发送的 SIP 客户端事务队列（PTZ、INVITE、BYE）
 * @details 在 PJSIP 工作线程中调用（SipServerPjsip worker）。顺序：先 drain 云台 MESSAGE，
 *          再 INVITE，最后 BYE，避免与点播会话状态交叉时产生竞态。
 * @note 函数名历史遗留含 Invites；新增 PTZ 不可再拆线程，须同线程串行发送
 */
void processPendingInvites() {
  std::vector<PendingPtz> ptzBatch;
  {
    std::lock_guard<std::mutex> lock(g_ptzMutex);
    ptzBatch.swap(g_pendingPtz);
  }
  for (const auto& p : ptzBatch) {
    if (!sendPtzDeviceControlMessage(p.platformGbId, p.channelId, p.ptzCmdHex)) {
      WarnL << "【processPendingInvites】PTZ DeviceControl failed platform=" << p.platformGbId
            << " channel=" << p.channelId;
    }
  }

  std::vector<PendingInvite> invites;
  {
    std::lock_guard<std::mutex> lock(g_invitesMutex);
    invites.swap(g_pendingInvites);
  }

  for (const auto& invite : invites) {
    std::string outSdp;
    std::string callId = invite.callId;  // 使用队列中生成的Call-ID

    InfoL << "【processPendingInvites】Sending INVITE for device " << invite.deviceGbId;

    bool result = sendPlayInvite(invite.deviceGbId, invite.channelId,
                                  invite.zlmPort, callId, outSdp,
                                  invite.sdpConnectionIp, invite.rtpOverTcp,
                                  invite.playbackStartUnix, invite.playbackEndUnix,
                                  invite.isDownload);

    if (result) {
      InfoL << "【processPendingInvites】INVITE sent successfully for " << invite.deviceGbId;
    } else {
      WarnL << "【processPendingInvites】Failed to send INVITE for " << invite.deviceGbId;
      upstreamBridgeOnDeviceInviteSendFailed(callId);
    }
  }

  // 处理待发送的BYE请求
  std::vector<PendingBye> byes;
  {
    std::lock_guard<std::mutex> lock(g_byesMutex);
    byes.swap(g_pendingByes);
  }

  for (const auto& bye : byes) {
    InfoL << "【processPendingInvites】Sending BYE for call " << bye.callId;

    bool result = sendPlayBye(bye.callId, bye.deviceGbId, bye.channelId);

    if (result) {
      InfoL << "【processPendingInvites】BYE sent successfully for call " << bye.callId;
    } else {
      WarnL << "【processPendingInvites】Failed to send BYE for call " << bye.callId;
    }
  }

  // 处理待发送的回放倍速 INFO 请求
  std::vector<PendingPlaybackSpeed> speedBatch;
  {
    std::lock_guard<std::mutex> lock(g_playbackSpeedMutex);
    speedBatch.swap(g_pendingPlaybackSpeed);
  }

  for (const auto& sp : speedBatch) {
    InfoL << "【processPendingInvites】Sending playback speed INFO Scale=" << sp.scale
          << " for call " << sp.callId;

    bool result = sendPlaybackSpeedInfo(sp.callId, sp.deviceGbId, sp.channelId, sp.scale);

    if (result) {
      InfoL << "【processPendingInvites】Speed INFO sent ok, Scale=" << sp.scale
            << " callId=" << sp.callId;
    } else {
      WarnL << "【processPendingInvites】Speed INFO failed, Scale=" << sp.scale
            << " callId=" << sp.callId;
    }
  }
}

/**
 * @brief 异步发送BYE请求停止点播
 * @details 将BYE请求加入队列，由PJSIP工作线程实际发送
 *          适用于从非PJSIP线程调用（如HTTP线程）
 */
bool sendPlayByeAsync(const std::string& callId,
                      const std::string& deviceGbId,
                      const std::string& channelId) {
  std::lock_guard<std::mutex> lock(g_byesMutex);

  PendingBye bye{callId, deviceGbId, channelId};
  g_pendingByes.push_back(bye);

  InfoL << "【sendPlayByeAsync】BYE queued for device " << deviceGbId
        << " channel " << channelId
        << " callId " << callId;
  return true;
}

/**
 * @brief 发送 SIP INFO 控制回放倍速（MANSRTSP PLAY + Scale）
 * @details GB28181-2016 Annex A.2.5：上级平台通过 INFO 向设备发送 MANSRTSP 回放控制。
 *          INFO 必须使用与 INVITE 建立会话相同的 Call-ID，使设备关联到正确的回放 dialog。
 * @param callId 回放 INVITE 会话的 Call-ID
 * @param deviceGbId 下级平台/设备国标 ID
 * @param channelId 通道/摄像机国标 ID
 * @param scale 回放倍速（0.25/0.5/1/2/4/8/16/32）
 * @return 是否成功发送
 */
bool sendPlaybackSpeedInfo(const std::string& callId,
                           const std::string& deviceGbId,
                           const std::string& channelId,
                           double scale) {
  InfoL << "【sendPlaybackSpeedInfo】Sending INFO Scale=" << scale
        << " to device " << deviceGbId
        << " channel " << channelId
        << " callId " << callId;

  if (!g_sip_endpt) {
    ErrorL << "【sendPlaybackSpeedInfo】SIP endpoint not initialized";
    return false;
  }

  std::string contactHost, signalHost;
  int contactPort = 5060, signalPort = 0;
  if (!loadPlatformSipRouting(deviceGbId, contactHost, contactPort, signalHost, signalPort)) {
    WarnL << "【sendPlaybackSpeedInfo】平台未入库: " << deviceGbId;
    return false;
  }
  std::string uriHost;
  int uriPort = 5060;
  if (!resolveUriHostForDownlink(deviceGbId, contactHost, contactPort, signalHost, signalPort,
                                 uriHost, uriPort, "【sendPlaybackSpeedInfo】")) {
    WarnL << "【sendPlaybackSpeedInfo】无可用信令地址: " << deviceGbId;
    return false;
  }
  const std::string sipContactLabel =
      contactUsableAsUriHost(contactHost)
          ? (contactHost + ":" + std::to_string(contactPort > 0 ? contactPort : 5060))
          : (std::string("degraded ") + uriHost + ":" + std::to_string(uriPort));

  std::string localHost = "127.0.0.1";
  int localPort = 5060;
  std::string localGbId = "34020000002000000001";

  std::string sql = "SELECT COALESCE(gb_id, '34020000002000000001') FROM gb_local_config LIMIT 1";
  std::string out = execPsql(sql.c_str());
  if (!out.empty()) {
    size_t nl = out.find('\n');
    if (nl != std::string::npos) out = out.substr(0, nl);
    size_t first = out.find_first_not_of(" \t\r\n");
    if (first != std::string::npos) localGbId = out.substr(first);
  }

  sql = "SELECT COALESCE(signal_ip, '127.0.0.1'), COALESCE(signal_port, 5060) FROM gb_local_config LIMIT 1";
  out = execPsql(sql.c_str());
  if (!out.empty()) {
    size_t pipePos = out.find('|');
    if (pipePos != std::string::npos) {
      localHost = out.substr(0, pipePos);
      size_t last = localHost.find_last_not_of(" \t\r\n");
      if (last != std::string::npos) localHost = localHost.substr(0, last + 1);
      std::string portStr = out.substr(pipePos + 1);
      size_t pf = portStr.find_first_not_of(" \t\r\n");
      if (pf != std::string::npos) portStr = portStr.substr(pf);
      if (!portStr.empty()) localPort = std::atoi(portStr.c_str());
    } else {
      localHost = out;
      size_t last = localHost.find_last_not_of(" \t\r\n");
      if (last != std::string::npos) localHost = localHost.substr(0, last + 1);
    }
  }
  if (localHost.empty()) {
    localHost = "127.0.0.1";
    WarnL << "【sendPlaybackSpeedInfo】gb_local_config.signal_ip 为空，回退 " << localHost;
  }

  pj_pool_t* pool = pjsip_endpt_create_pool(g_sip_endpt, "info_speed", 2048, 1024);
  if (!pool) {
    ErrorL << "【sendPlaybackSpeedInfo】Failed to create pool";
    return false;
  }

  std::string targetUriStr = "sip:" + channelId + "@" + uriHost + ":" + std::to_string(uriPort);
  pj_str_t targetUriPj = pj_str((char*)targetUriStr.c_str());
  pjsip_uri* target_uri = pjsip_parse_uri(pool, targetUriPj.ptr, targetUriPj.slen, PJSIP_PARSE_URI_AS_NAMEADDR);
  if (!target_uri) {
    ErrorL << "【sendPlaybackSpeedInfo】Failed to parse target URI: " << targetUriStr;
    pjsip_endpt_release_pool(g_sip_endpt, pool);
    return false;
  }

  pjsip_from_hdr* from_hdr = pjsip_from_hdr_create(pool);
  if (from_hdr) {
    pjsip_name_addr* from_name_addr = pjsip_name_addr_create(pool);
    if (from_name_addr) {
      pjsip_sip_uri* from_uri = pjsip_sip_uri_create(pool, PJ_FALSE);
      if (from_uri) {
        pj_strdup2(pool, &from_uri->user, (char*)localGbId.c_str());
        pj_strdup2(pool, &from_uri->host, (char*)localHost.c_str());
        from_uri->port = localPort;
        from_name_addr->uri = (pjsip_uri*)from_uri;
      }
      from_hdr->uri = (pjsip_uri*)from_name_addr;
    }
  }

  pjsip_to_hdr* to_hdr = pjsip_to_hdr_create(pool);
  if (to_hdr) {
    pjsip_name_addr* to_name_addr = pjsip_name_addr_create(pool);
    if (to_name_addr) {
      pjsip_sip_uri* to_uri = pjsip_sip_uri_create(pool, PJ_FALSE);
      if (to_uri) {
        pj_strdup2(pool, &to_uri->user, (char*)channelId.c_str());
        pj_strdup2(pool, &to_uri->host, (char*)uriHost.c_str());
        to_uri->port = uriPort;
        to_name_addr->uri = (pjsip_uri*)to_uri;
      }
      to_hdr->uri = (pjsip_uri*)to_name_addr;
    }
  }

  /* INFO 不是标准 PJSIP 内置方法，需自定义 */
  pj_str_t info_method_name = pj_str((char*)"INFO");
  pjsip_method info_method;
  pjsip_method_init_np(&info_method, &info_method_name);

  pjsip_tx_data* tdata = nullptr;
  pj_status_t st = pjsip_endpt_create_request_from_hdr(g_sip_endpt,
                                                        &info_method,
                                                        target_uri,
                                                        from_hdr,
                                                        to_hdr,
                                                        nullptr,
                                                        nullptr,
                                                        -1,
                                                        nullptr,
                                                        &tdata);
  if (st != PJ_SUCCESS || !tdata) {
    ErrorL << "【sendPlaybackSpeedInfo】Failed to create INFO request: " << st;
    pjsip_endpt_release_pool(g_sip_endpt, pool);
    return false;
  }

  presetTxDataUdpDestFromSignal(tdata, signalHost, signalPort, "【sendPlaybackSpeedInfo】", sipContactLabel);

  /* 设置与回放 INVITE 相同的 Call-ID，使设备关联到同一 dialog */
  pjsip_cid_hdr* call_id_hdr = (pjsip_cid_hdr*)pjsip_msg_find_hdr(tdata->msg, PJSIP_H_CALL_ID, nullptr);
  if (call_id_hdr) {
    pj_strdup2(pool, &call_id_hdr->id, callId.c_str());
  }

  /* Via 分支 */
  static int infoCounter = 0;
  infoCounter++;
  pjsip_via_hdr* via = (pjsip_via_hdr*)pjsip_msg_find_hdr(tdata->msg, PJSIP_H_VIA, nullptr);
  if (via) {
    pj_strdup2(pool, &via->transport, (char*)"UDP");
    pj_strdup2(pool, &via->sent_by.host, (char*)localHost.c_str());
    via->sent_by.port = localPort;
    char branch_buf[64];
    snprintf(branch_buf, sizeof(branch_buf), "z9hG4bKinfo%d", infoCounter);
    pj_strdup2(pool, &via->branch_param, branch_buf);
  }

  /* 构造 MANSRTSP body：PLAY 指令 + Scale + Range */
  int cseq = g_infoCSeqCounter.fetch_add(1);
  char scaleBuf[32];
  if (scale == (int)scale) {
    snprintf(scaleBuf, sizeof(scaleBuf), "%.1f", scale);
  } else {
    snprintf(scaleBuf, sizeof(scaleBuf), "%.2f", scale);
  }
  std::string mansrtspBody =
      "PLAY MANSRTSP/1.0\r\n"
      "CSeq: " + std::to_string(cseq) + "\r\n"
      "Scale: " + std::string(scaleBuf) + "\r\n"
      "Range: npt=now-\r\n";

  pj_str_t type_str = pj_str((char*)"Application");
  pj_str_t subtype_str = pj_str((char*)"MANSRTSP");
  pj_str_t body_str = pj_str((char*)mansrtspBody.c_str());
  tdata->msg->body = pjsip_msg_body_create(pool, &type_str, &subtype_str, &body_str);

  char msg_buf[2048];
  pj_ssize_t msg_len = pjsip_msg_print(tdata->msg, msg_buf, sizeof(msg_buf));
  if (msg_len > 0) {
    InfoL << "【sendPlaybackSpeedInfo】SIP INFO消息:\n" << std::string(msg_buf, msg_len);
  }

  st = pjsip_endpt_send_request_stateless(g_sip_endpt, tdata, nullptr, nullptr);
  if (st != PJ_SUCCESS) {
    ErrorL << "【sendPlaybackSpeedInfo】Failed to send INFO: " << st;
    pjsip_endpt_release_pool(g_sip_endpt, pool);
    return false;
  }

  InfoL << "【sendPlaybackSpeedInfo】INFO sent successfully, Scale=" << scaleBuf
        << " callId=" << callId;
  pjsip_endpt_release_pool(g_sip_endpt, pool);
  return true;
}

/**
 * @brief 异步入队回放倍速 INFO（供 HTTP 等线程调用）
 * @param callId 回放会话 Call-ID
 * @param deviceGbId 下级平台/设备国标 ID
 * @param channelId 通道 ID
 * @param scale 目标倍速
 * @return 入队成功 true
 */
bool sendPlaybackSpeedInfoAsync(const std::string& callId,
                                const std::string& deviceGbId,
                                const std::string& channelId,
                                double scale) {
  std::lock_guard<std::mutex> lock(g_playbackSpeedMutex);

  /* 对同一 callId 去重：若队列中已有该 callId 的倍速指令，覆盖为最新 scale */
  for (auto& p : g_pendingPlaybackSpeed) {
    if (p.callId == callId) {
      InfoL << "【sendPlaybackSpeedInfoAsync】更新队列中已有项 Scale=" << scale
            << " callId=" << callId;
      p.scale = scale;
      return true;
    }
  }

  PendingPlaybackSpeed item{callId, deviceGbId, channelId, scale};
  g_pendingPlaybackSpeed.push_back(item);

  InfoL << "【sendPlaybackSpeedInfoAsync】INFO Speed queued Scale=" << scale
        << " device=" << deviceGbId
        << " channel=" << channelId
        << " callId=" << callId;
  return true;
}

/**
 * @brief 将云台控制请求入队（供 HTTP 等线程调用）
 * @param platformGbId 下级平台国标 ID
 * @param channelId 摄像机/通道国标 ID
 * @param command 方向/变焦/光圈/stop，见头文件说明
 * @param action start 或 stop
 * @param speed 1–3
 * @return 入队成功 true；SIP 未就绪、参数空、无法编码指令时为 false
 * @details 仅将任务写入 g_pendingPtz；实际发包见 processPendingInvites → sendPtzDeviceControlMessage
 * @note 线程安全：内持 g_ptzMutex；勿在回调内长时间持锁
 */
bool enqueuePtzDeviceControl(const std::string& platformGbId,
                             const std::string& channelId,
                             const std::string& command,
                             const std::string& action,
                             int speed) {
  if (!g_sip_endpt) {
    WarnL << "【enqueuePtzDeviceControl】SIP endpoint not ready";
    return false;
  }
  if (platformGbId.empty() || channelId.empty()) return false;

  std::string hex = gb28181BuildPtzCmdHex(command, action, speed);
  if (hex.empty()) {
    WarnL << "【enqueuePtzDeviceControl】Unknown PTZ command=" << command << " action=" << action;
    return false;
  }

  std::lock_guard<std::mutex> lock(g_ptzMutex);
  g_pendingPtz.push_back(PendingPtz{platformGbId, channelId, std::move(hex)});
  InfoL << "【enqueuePtzDeviceControl】queued platform=" << platformGbId << " channel=" << channelId;
  return true;
}

bool enqueuePtzDeviceControlHex(const std::string& platformGbId,
                                const std::string& channelId,
                                const std::string& ptzCmdHex) {
  if (!g_sip_endpt) {
    WarnL << "【enqueuePtzDeviceControlHex】SIP endpoint not ready";
    return false;
  }
  if (platformGbId.empty() || channelId.empty()) return false;
  std::string hex = ptzCmdHex;
  for (char& c : hex) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  if (hex.size() != 16) {
    WarnL << "【enqueuePtzDeviceControlHex】Invalid PTZCmd length=" << hex.size();
    return false;
  }
  for (char c : hex) {
    if (!std::isxdigit(static_cast<unsigned char>(c))) {
      WarnL << "【enqueuePtzDeviceControlHex】Invalid PTZCmd char";
      return false;
    }
  }
  std::lock_guard<std::mutex> lock(g_ptzMutex);
  g_pendingPtz.push_back(PendingPtz{platformGbId, channelId, std::move(hex)});
  InfoL << "【enqueuePtzDeviceControlHex】queued platform=" << platformGbId << " channel=" << channelId;
  return true;
}

bool sendUpstreamManscdpMessage(const std::string& destSignalIp,
                                  int destSignalPort,
                                  const std::string& toUser,
                                  const std::string& toHost,
                                  int toPort,
                                  const std::string& xmlBody,
                                  const std::string& sipMethod) {
  if (!g_sip_endpt || destSignalIp.empty() || destSignalPort <= 0 || xmlBody.empty()) return false;
  if (toUser.empty() || toHost.empty() || toPort <= 0) return false;

  std::string localGbId = getSystemGbId();
  if (localGbId.empty()) localGbId = "34020000002000000001";

  int localPort = 5060;
  std::string localHost = "127.0.0.1";
  std::string out = execPsql(
      "SELECT COALESCE(NULLIF(TRIM(signal_ip),''), '127.0.0.1'), COALESCE(signal_port, 5060) FROM "
      "gb_local_config LIMIT 1");
  if (!out.empty()) {
    size_t pipePos = out.find('|');
    if (pipePos != std::string::npos) {
      localHost = trimSqlField(out.substr(0, pipePos));
      std::string portStr = trimSqlField(out.substr(pipePos + 1));
      if (!portStr.empty()) localPort = std::atoi(portStr.c_str());
    }
  }
  if (localHost.empty()) localHost = "127.0.0.1";

  std::string targetUriStr = "sip:" + toUser + "@" + toHost + ":" + std::to_string(toPort);
  std::string localUriStr = "sip:" + localGbId + "@" + localHost + ":" + std::to_string(localPort);

  pj_pool_t* pool = pjsip_endpt_create_pool(g_sip_endpt, "upstream_msg", 2048, 1024);
  if (!pool) return false;

  pj_str_t target_str = pj_str((char*)targetUriStr.c_str());
  pjsip_uri* target_uri = pjsip_parse_uri(pool, target_str.ptr, target_str.slen, 0);
  if (!target_uri) {
    pjsip_endpt_release_pool(g_sip_endpt, pool);
    return false;
  }

  pjsip_from_hdr* from_hdr = pjsip_from_hdr_create(pool);
  if (from_hdr) {
    pjsip_name_addr* na = pjsip_name_addr_create(pool);
    if (na) {
      pjsip_sip_uri* u = pjsip_sip_uri_create(pool, PJ_FALSE);
      if (u) {
        pj_strdup2(pool, &u->user, (char*)localGbId.c_str());
        pj_strdup2(pool, &u->host, (char*)localHost.c_str());
        u->port = localPort;
        na->uri = (pjsip_uri*)u;
      }
      from_hdr->uri = (pjsip_uri*)na;
    }
  }

  pjsip_to_hdr* to_hdr = pjsip_to_hdr_create(pool);
  if (to_hdr) {
    pjsip_name_addr* na = pjsip_name_addr_create(pool);
    if (na) {
      pjsip_sip_uri* u = pjsip_sip_uri_create(pool, PJ_FALSE);
      if (u) {
        pj_strdup2(pool, &u->user, (char*)toUser.c_str());
        pj_strdup2(pool, &u->host, (char*)toHost.c_str());
        u->port = (pj_uint16_t)toPort;
        na->uri = (pjsip_uri*)u;
      }
      to_hdr->uri = (pjsip_uri*)na;
    }
  }

  std::string methodBuf = sipMethod.empty() ? "MESSAGE" : sipMethod;
  pj_str_t method_str = pj_str((char*)methodBuf.c_str());
  pjsip_method method;
  pjsip_method_init_np(&method, &method_str);

  static int upstreamMsgCseq = 1;
  pjsip_tx_data* tdata = nullptr;
  pj_status_t st = pjsip_endpt_create_request_from_hdr(g_sip_endpt, &method, target_uri, from_hdr, to_hdr,
                                                       nullptr, nullptr, upstreamMsgCseq++, nullptr, &tdata);
  if (st != PJ_SUCCESS || !tdata) {
    pjsip_endpt_release_pool(g_sip_endpt, pool);
    return false;
  }

  pjsip_endpt_release_pool(g_sip_endpt, pool);
  pool = nullptr;

  presetTxDataUdpDestFromSignal(tdata, destSignalIp, destSignalPort, "【UPSTREAM-MSG】",
                                toHost + ":" + std::to_string(toPort));

  pj_pool_t* msg_pool = tdata->pool;
  pjsip_via_hdr* via = (pjsip_via_hdr*)pjsip_msg_find_hdr(tdata->msg, PJSIP_H_VIA, nullptr);
  if (via) {
    pj_strdup2(msg_pool, &via->transport, (char*)"UDP");
    pj_strdup2(msg_pool, &via->sent_by.host, (char*)localHost.c_str());
    via->sent_by.port = localPort;
  }

  pj_str_t body_str = pj_str((char*)xmlBody.c_str());
  pj_str_t type_str = pj_str((char*)"Application");
  pj_str_t subtype_str = pj_str((char*)"MANSCDP+xml");
  tdata->msg->body = pjsip_msg_body_create(msg_pool, &type_str, &subtype_str, &body_str);

  st = pjsip_endpt_send_request_stateless(g_sip_endpt, tdata, nullptr, nullptr);
  if (st != PJ_SUCCESS) {
    pjsip_tx_data_dec_ref(tdata);
    return false;
  }
  return true;
}

}  // namespace gb
