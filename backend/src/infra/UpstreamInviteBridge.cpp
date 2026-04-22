/**
 * @file UpstreamInviteBridge.cpp
 * @brief 上级入站 INVITE：编组对外 ID 映射、openRtpServer、向下级 INVITE、ZLM startSendRtp 回上级
 *        上级收流 UDP/TCP 与 200 SDP 随上级 Offer；向下级 INVITE 的 RTP 传输仅随平台策略（resolveSdpConnForPlatform）
 */
#include "infra/UpstreamInviteBridge.h"
#include "infra/UpstreamRegistrar.h"
#include "infra/UpstreamPlatformService.h"
#include "infra/MediaService.h"
#include "infra/SipCatalog.h"
#include "infra/DbUtil.h"
#include "infra/AuthHelper.h"
#include "infra/SipServerPjsip.h"
#include "Util/logger.h"

#include <pjsip.h>
#include <pjsip/sip_transport.h>
#include <pjlib.h>

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <unordered_map>

#include <cstdlib>

using namespace toolkit;

namespace gb {

extern pjsip_endpoint* g_sip_endpt;

// 从 SipServerPjsip.cpp 前向声明
extern void extractSignalSrcFromRdata(pjsip_rx_data* rdata, std::string& outIp, int& outPort);

struct UpstreamBridgeCtx {
  std::string upstreamCallId;
  pjsip_rx_data* upstreamInviteClone{nullptr};
  std::string deviceCallId;
  std::string streamId;
  std::string sessionId;
  std::string platformGbId;
  std::string channelGbId;
  std::string catalogGbId;
  uint16_t zlmPort{0};
  std::string ssrc;
  std::string sendDstIp;
  uint16_t sendDstPort{0};
  bool offerTcp{false};
  bool answeredUpstream{false};
  /** 下级 200 已收，等待 ZLM 注册源流后再 startSendRtp 答上级 */
  bool waitingZlmSource{false};
  std::chrono::steady_clock::time_point zlmSourceWaitUntil{};

  /** SIP 对话字段，用于上级 BYE */
  std::string upstreamFromUri;       // INVITE From 头（含 tag）
  std::string upstreamFromTag;       // From tag
  std::string upstreamToTag;         // 200 OK 生成的 To-tag
  std::string upstreamLocalUri;      // 本级 GB ID（From URI）
  std::string upstreamSignalIp;      // 上级信令源 IP
  uint16_t upstreamSignalPort{0};    // 上级信令源端口
};

std::mutex g_mtx;
std::unordered_map<std::string, std::shared_ptr<UpstreamBridgeCtx>> g_byUpstreamCallId;
std::unordered_map<std::string, std::weak_ptr<UpstreamBridgeCtx>> g_byDeviceCallId;
std::mutex g_replay_hint_mu;
std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_replay_hints;

static std::string makeReplayHintKey(int64_t upstreamDbId, const std::string& catalogGbId) {
  return std::to_string(upstreamDbId) + "|" + catalogGbId;
}

static void cleanupReplayHintsLocked(std::chrono::steady_clock::time_point now) {
  for (auto it = g_replay_hints.begin(); it != g_replay_hints.end();) {
    if (now > it->second) {
      it = g_replay_hints.erase(it);
    } else {
      ++it;
    }
  }
}

static bool consumeReplayHint(int64_t upstreamDbId, const std::string& catalogGbId) {
  if (upstreamDbId <= 0 || catalogGbId.empty()) return false;
  auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lk(g_replay_hint_mu);
  cleanupReplayHintsLocked(now);
  const std::string key = makeReplayHintKey(upstreamDbId, catalogGbId);
  auto it = g_replay_hints.find(key);
  if (it == g_replay_hints.end()) return false;
  g_replay_hints.erase(it);
  return true;
}

std::string trimField(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

void resolveSdpConnForPlatform(const std::string& platformGbId, std::string& outIp, bool& outTcp) {
  outTcp = false;
  outIp = "127.0.0.1";
  std::string q = execPsql(
      "SELECT COALESCE(NULLIF(TRIM(media_http_host),''), '127.0.0.1'), "
      "LOWER(COALESCE(NULLIF(TRIM(rtp_transport),''), 'udp')) "
      "FROM media_config WHERE id = 1 LIMIT 1");
  if (!q.empty()) {
    size_t nl = q.find('\n');
    std::string line = (nl == std::string::npos) ? q : q.substr(0, nl);
    size_t pipe = line.find('|');
    std::string h = trimField(pipe == std::string::npos ? line : line.substr(0, pipe));
    if (!h.empty()) outIp = h;
    if (pipe != std::string::npos) {
      std::string t = trimField(line.substr(pipe + 1));
      if (t == "tcp") outTcp = true;
    }
  }
  std::string platSql =
      "SELECT strategy_mode, COALESCE(stream_media_url,''), "
      "LOWER(COALESCE(stream_rtp_transport::text, '')) "
      "FROM device_platforms WHERE gb_id='" +
      escapeSqlString(platformGbId) + "' LIMIT 1";
  std::string pq = execPsql(platSql.c_str());
  if (pq.empty()) return;
  size_t nl = pq.find('\n');
  std::string line = (nl == std::string::npos) ? pq : pq.substr(0, nl);
  size_t p1 = line.find('|');
  if (p1 == std::string::npos) return;
  size_t p2 = line.find('|', p1 + 1);
  std::string strategy = trimField(line.substr(0, p1));
  std::string streamUrl =
      p2 == std::string::npos ? trimField(line.substr(p1 + 1)) : trimField(line.substr(p1 + 1, p2 - p1 - 1));
  std::string platRtp;
  if (p2 != std::string::npos) platRtp = trimField(line.substr(p2 + 1));
  if (strategy != "custom") return;
  if (!streamUrl.empty()) {
    size_t proto = streamUrl.find("://");
    std::string rest = proto == std::string::npos ? streamUrl : streamUrl.substr(proto + 3);
    size_t colon = rest.find(':');
    size_t slash = rest.find('/');
    size_t end = std::min(colon == std::string::npos ? rest.size() : colon,
                         slash == std::string::npos ? rest.size() : slash);
    std::string hostFromUrl = trimField(rest.substr(0, end));
    if (!hostFromUrl.empty()) outIp = hostFromUrl;
  }
  if (platRtp == "tcp")
    outTcp = true;
  else if (platRtp == "udp")
    outTcp = false;
}

std::string headerValueByName(pjsip_msg* msg, const char* name) {
  if (!msg) return "";
  pj_str_t pname = pj_str((char*)name);
  pjsip_hdr* h = (pjsip_hdr*)pjsip_msg_find_hdr_by_name(msg, &pname, nullptr);
  if (!h) return "";
  char buf[1024];
  int n = pjsip_hdr_print_on(h, buf, (int)sizeof(buf) - 1);
  if (n <= 0) return "";
  buf[n] = '\0';
  std::string line(buf);
  size_t c = line.find(':');
  if (c == std::string::npos) return trimField(line);
  return trimField(line.substr(c + 1));
}

std::string parseSubjectChannel(const std::string& subj) {
  if (subj.empty()) return "";
  size_t comma = subj.find(',');
  std::string first = comma == std::string::npos ? subj : subj.substr(0, comma);
  size_t colon = first.find(':');
  std::string id = colon == std::string::npos ? first : first.substr(0, colon);
  return trimField(id);
}

bool parseOfferVideoDest(const std::string& sdp, std::string& outIp, uint16_t& outPort, bool& outTcp) {
  outTcp = false;
  std::string conn = "0.0.0.0";
  std::istringstream in(sdp);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.size() >= 9 && line.compare(0, 9, "c=IN IP4 ") == 0) {
      conn = trimField(line.substr(9));
    } else if (line.size() >= 8 && line.compare(0, 8, "m=video ") == 0) {
      if (line.find("TCP") != std::string::npos) outTcp = true;
      size_t sp = 8;
      size_t sp2 = line.find(' ', sp);
      if (sp2 == std::string::npos) continue;
      int p = std::atoi(line.substr(sp, sp2 - sp).c_str());
      if (p > 0 && p < 65536) {
        outIp = conn;
        outPort = static_cast<uint16_t>(p);
        return true;
      }
    }
  }
  return false;
}

void parsePlaybackUnix(const std::string& sdp, uint64_t& t0, uint64_t& t1) {
  t0 = t1 = 0;
  if (sdp.find("s=Playback") == std::string::npos && sdp.find("s=Download") == std::string::npos) return;
  size_t pos = 0;
  while ((pos = sdp.find("t=", pos)) != std::string::npos) {
    pos += 2;
    unsigned long long a = 0, b = 0;
    if (std::sscanf(sdp.c_str() + pos, "%llu %llu", &a, &b) >= 2) {
      t0 = static_cast<uint64_t>(a);
      t1 = static_cast<uint64_t>(b);
      return;
    }
    pos++;
  }
}

std::string extractYssrc(const std::string& sdp) {
  size_t p = sdp.find("\ny=");
  if (p == std::string::npos) p = sdp.find("y=");
  if (p == std::string::npos) return "";
  p += 2;
  size_t e = p;
  while (e < sdp.size() && std::isdigit(static_cast<unsigned char>(sdp[e]))) e++;
  return sdp.substr(p, e - p);
}

/** RTP SSRC 为 32 位；Subject 里 20 位平台国标 id 等会误判，须排除 */
static bool isValidGbRtpSsrcDecimalString(const std::string& s) {
  if (s.empty() || s.size() > 10) {
    return false;
  }
  for (char c : s) {
    if (!std::isdigit(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  const unsigned long long v = std::strtoull(s.c_str(), nullptr, 10);
  return v <= 4294967295ULL;
}

/** 与 SipCatalog::sendPlayInvite 一致：国标常用 10 位十进制 SSRC */
static std::string generateOutboundRtpSsrcDecimal() {
  static std::random_device rd;
  static std::mt19937 gen(rd());
  static std::uniform_int_distribution<uint32_t> dis(1000000000u, 4294967295u);
  return std::to_string(dis(gen));
}

void sendSimpleResp(pjsip_rx_data* rdata, int code) {
  pjsip_tx_data* tdata = nullptr;
  if (pjsip_endpt_create_response(g_sip_endpt, rdata, code, nullptr, &tdata) == PJ_SUCCESS && tdata) {
    pjsip_endpt_send_response2(g_sip_endpt, rdata, tdata, nullptr, nullptr);
  }
}

/** 发送 200 OK SDP 应答，返回生成的 To-tag（供 BYE 使用） */
std::string sendSdpAnswer(pjsip_rx_data* rdata, const std::string& sdp) {
  pjsip_tx_data* tdata = nullptr;
  pj_status_t st = pjsip_endpt_create_response(g_sip_endpt, rdata, 200, nullptr, &tdata);
  if (st != PJ_SUCCESS || !tdata) {
    sendSimpleResp(rdata, 500);
    return "";
  }

  // 生成 To-tag
  static std::atomic<int> toTagCounter{0};
  pj_timestamp ts;
  pj_get_timestamp(&ts);
  std::string toTag = "gb" + std::to_string(++toTagCounter) + "z" + std::to_string(ts.u32.lo);

  // 设置 To 头的 tag
  pjsip_to_hdr* to_hdr = (pjsip_to_hdr*)pjsip_msg_find_hdr(tdata->msg, PJSIP_H_TO, nullptr);
  if (to_hdr) {
    pj_str_t tag_str = pj_str((char*)toTag.c_str());
    pj_strdup(tdata->pool, &to_hdr->tag, &tag_str);
  }

  pj_pool_t* pool = tdata->pool;
  pj_str_t type_str = pj_str((char*)"application");
  pj_str_t subtype_str = pj_str((char*)"sdp");
  char* buf = (char*)pj_pool_alloc(pool, sdp.size() + 1);
  if (!buf) {
    sendSimpleResp(rdata, 500);
    return "";
  }
  memcpy(buf, sdp.c_str(), sdp.size());
  buf[sdp.size()] = '\0';
  pj_str_t body_str;
  body_str.ptr = buf;
  body_str.slen = (int)sdp.size();
  tdata->msg->body = pjsip_msg_body_create(pool, &type_str, &subtype_str, &body_str);
  pjsip_endpt_send_response2(g_sip_endpt, rdata, tdata, nullptr, nullptr);
  return toTag;
}

/** ZLM 已有流或本地会话已分配 zlm 收流端口：向上级 200 + startSendRtp（ctx 不设 sessionId，避免 BYE 误关预览会话） */
enum class UpstreamDirectAnswer { Ok, BadRequest, StartSendRtpFailed };

/** 从 INVITE 解析并保存 SIP 对话字段到 ctx */
static void extractUpstreamSipDialog(pjsip_rx_data* rdata, pjsip_msg* msg, UpstreamBridgeCtx& ctx) {
  // 解析 Call-ID
  pjsip_cid_hdr* cid = (pjsip_cid_hdr*)pjsip_msg_find_hdr(msg, PJSIP_H_CALL_ID, nullptr);
  if (cid && cid->id.slen > 0) {
    ctx.upstreamCallId = std::string(cid->id.ptr, (size_t)cid->id.slen);
  }

  // 解析 From 头（含 tag）
  pjsip_from_hdr* from = (pjsip_from_hdr*)pjsip_msg_find_hdr(msg, PJSIP_H_FROM, nullptr);
  if (from) {
    char buf[256];
    int len = pjsip_uri_print(PJSIP_URI_IN_FROMTO_HDR, from->uri, buf, sizeof(buf) - 1);
    if (len > 0) {
      ctx.upstreamFromUri = std::string(buf, len);
    }
    if (from->tag.slen > 0) {
      ctx.upstreamFromTag = std::string(from->tag.ptr, (size_t)from->tag.slen);
    }
  }

  // 保存本地信令地址（用于 Via 和 UDP 目的）
  std::string sigIp;
  int sigPort = 0;
  extractSignalSrcFromRdata(rdata, sigIp, sigPort);
  ctx.upstreamSignalIp = sigIp;
  ctx.upstreamSignalPort = static_cast<uint16_t>(sigPort);

  // 获取本级 GB ID
  std::string sql = "SELECT COALESCE(gb_id, '34020000002000000001') FROM gb_local_config LIMIT 1";
  std::string out = execPsql(sql.c_str());
  if (!out.empty()) {
    size_t nl = out.find('\n');
    if (nl != std::string::npos) out = out.substr(0, nl);
    ctx.upstreamLocalUri = trimField(out);
  }
  if (ctx.upstreamLocalUri.empty()) {
    ctx.upstreamLocalUri = "34020000002000000001";
  }
}

static UpstreamDirectAnswer tryAnswerUpstreamWithExistingZlm(pjsip_rx_data* rdata,
                                                             pjsip_msg* msg,
                                                             MediaService& media,
                                                             const std::string& catalogId,
                                                             const std::string& expectedStreamId,
                                                             const std::string& platGb,
                                                             const std::string& devGb,
                                                             const std::string& dstIp,
                                                             uint16_t dstPort,
                                                             bool upstreamOfferTcp) {
  pjsip_cid_hdr* ucid = (pjsip_cid_hdr*)pjsip_msg_find_hdr(msg, PJSIP_H_CALL_ID, nullptr);
  if (!ucid || ucid->id.slen <= 0) return UpstreamDirectAnswer::BadRequest;
  std::string upstreamCallId(ucid->id.ptr, (size_t)ucid->id.slen);

  /* ZLM startSendRtp 的 ssrc = 推往对端的 RTP SSRC，须与答上级的 200 SDP 中 y= 一致；不得用 Subject 里平台国标 id */
  std::string offerSdp;
  if (msg->body && msg->body->data && msg->body->len > 0) {
    offerSdp.assign((const char*)msg->body->data, (size_t)msg->body->len);
  }
  std::string outboundSsrc = extractYssrc(offerSdp);
  if (!isValidGbRtpSsrcDecimalString(outboundSsrc)) {
    outboundSsrc = generateOutboundRtpSsrcDecimal();
    InfoL << "【UpstreamInviteBridge】上级 INVITE 无有效 y=，生成本级推上级 RTP SSRC=" << outboundSsrc
          << "（与 200 SDP 中 y= 一致）";
  } else {
    InfoL << "【UpstreamInviteBridge】采用上级 INVITE SDP 的 y= 作为推上级 RTP SSRC=" << outboundSsrc;
  }

  uint16_t localPort = 0;
  const bool zlmUdpToUpstream = !upstreamOfferTcp;
  if (!media.startSendRtpPs(expectedStreamId, outboundSsrc, dstIp, dstPort, zlmUdpToUpstream, localPort))
    return UpstreamDirectAnswer::StartSendRtpFailed;

  std::string mediaIp;
  bool unusedTcp = false;
  resolveSdpConnForPlatform(platGb, mediaIp, unusedTcp);

  std::ostringstream sdpAns;
  sdpAns << "v=0\r\n";
  sdpAns << "o=" << catalogId << " 0 0 IN IP4 " << mediaIp << "\r\n";
  sdpAns << "s=Play\r\n";
  sdpAns << "c=IN IP4 " << mediaIp << "\r\n";
  sdpAns << "t=0 0\r\n";
  if (upstreamOfferTcp) {
    sdpAns << "m=video " << localPort << " TCP/RTP/AVP 96\r\n";
    sdpAns << "a=setup:passive\r\n";
    sdpAns << "a=connection:new\r\n";
  } else {
    sdpAns << "m=video " << localPort << " RTP/AVP 96\r\n";
  }
  sdpAns << "a=rtpmap:96 PS/90000\r\n";
  sdpAns << "a=sendonly\r\n";
  sdpAns << "y=" << outboundSsrc << "\r\n";

  std::string toTag = sendSdpAnswer(rdata, sdpAns.str());

  auto ctx = std::make_shared<UpstreamBridgeCtx>();
  ctx->upstreamCallId = upstreamCallId;
  ctx->streamId = expectedStreamId;
  ctx->catalogGbId = catalogId;
  ctx->platformGbId = platGb;
  ctx->channelGbId = devGb;
  ctx->sendDstIp = dstIp;
  ctx->sendDstPort = dstPort;
  ctx->answeredUpstream = true;
  ctx->ssrc = outboundSsrc;
  ctx->upstreamToTag = toTag;
  extractUpstreamSipDialog(rdata, msg, *ctx);

  {
    std::lock_guard<std::mutex> lock(g_mtx);
    g_byUpstreamCallId[upstreamCallId] = ctx;
  }

  InfoL << "【UpstreamInviteBridge】直接推上级 streamId=" << expectedStreamId << " local_port=" << localPort << " → "
        << dstIp << ":" << dstPort << " upstream_media=" << (upstreamOfferTcp ? "TCP" : "UDP");
  return UpstreamDirectAnswer::Ok;
}

void eraseBridgeLocked(const std::shared_ptr<UpstreamBridgeCtx>& b) {
  if (!b) return;
  g_byUpstreamCallId.erase(b->upstreamCallId);
  g_byDeviceCallId.erase(b->deviceCallId);
}

void teardownBridge(std::shared_ptr<UpstreamBridgeCtx> b, const char* reason) {
  if (!b) return;
  InfoL << "【UpstreamInviteBridge】teardown: " << (reason ? reason : "") << " upstreamCall=" << b->upstreamCallId;
  if (!b->answeredUpstream && b->upstreamInviteClone) {
    sendSimpleResp(b->upstreamInviteClone, 486);
    pjsip_rx_data_free_cloned(b->upstreamInviteClone);
    b->upstreamInviteClone = nullptr;
  } else if (b->upstreamInviteClone) {
    pjsip_rx_data_free_cloned(b->upstreamInviteClone);
    b->upstreamInviteClone = nullptr;
  }
  auto& media = MediaService::instance();
  if (!b->ssrc.empty()) {
    media.stopSendRtpForStream(b->streamId, b->ssrc);
  }
  if (!b->sessionId.empty()) {
    if (!b->deviceCallId.empty()) {
      sendPlayByeAsync(b->deviceCallId, b->platformGbId, b->channelGbId);
    }
    media.closeSession(b->sessionId);
  } else if (b->zlmPort > 0 && !b->streamId.empty()) {
    media.closeRtpServer(b->streamId);
  }
  std::lock_guard<std::mutex> lock(g_mtx);
  eraseBridgeLocked(b);
}

/** 向上级平台发送 SIP BYE（同步发送） */
static bool sendUpstreamPlatformBye(const UpstreamBridgeCtx& ctx) {
  if (ctx.upstreamCallId.empty() || ctx.upstreamFromUri.empty()) {
    WarnL << "【sendUpstreamPlatformBye】缺少必要字段，无法发送 BYE";
    return false;
  }

  InfoL << "【sendUpstreamPlatformBye】向上级发送 BYE callId=" << ctx.upstreamCallId
        << " stream=" << ctx.streamId;

  if (!g_sip_endpt) {
    ErrorL << "【sendUpstreamPlatformBye】SIP endpoint not initialized";
    return false;
  }

  // 获取本机配置
  std::string localHost = "127.0.0.1";
  int localPort = 5060;
  std::string localGbId = ctx.upstreamLocalUri.empty() ? "34020000002000000001" : ctx.upstreamLocalUri;

  std::string sql = "SELECT COALESCE(signal_ip, '127.0.0.1'), COALESCE(signal_port, 5060) FROM gb_local_config LIMIT 1";
  std::string out = execPsql(sql.c_str());
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
  if (localHost.empty()) localHost = "127.0.0.1";

  // 创建内存池
  pj_pool_t* pool = pjsip_endpt_create_pool(g_sip_endpt, "bye_upstream", 2048, 1024);
  if (!pool) {
    ErrorL << "【sendUpstreamPlatformBye】Failed to create pool";
    return false;
  }

  // Request-URI：使用上级 From URI 的 host:port 部分
  std::string targetUriStr = ctx.upstreamFromUri;
  // 如果 FromUri 包含 sip: 前缀，解析出 user@host:port
  if (targetUriStr.find("sip:") == 0) {
    targetUriStr = targetUriStr.substr(4); // 去掉 sip:
  }
  // 构造目标 URI（使用上级信令地址）
  std::string destHost = ctx.upstreamSignalIp.empty() ? localHost : ctx.upstreamSignalIp;
  uint16_t destPort = ctx.upstreamSignalPort == 0 ? 5060 : ctx.upstreamSignalPort;
  targetUriStr = "sip:" + localGbId + "@" + destHost + ":" + std::to_string(destPort);

  pj_str_t targetUriPj = pj_str((char*)targetUriStr.c_str());
  pjsip_uri* target_uri = pjsip_parse_uri(pool, targetUriPj.ptr, targetUriPj.slen, PJSIP_PARSE_URI_AS_NAMEADDR);
  if (!target_uri) {
    ErrorL << "【sendUpstreamPlatformBye】Failed to parse target URI: " << targetUriStr;
    pjsip_endpt_release_pool(g_sip_endpt, pool);
    return false;
  }

  // 创建 From 头（使用本级标识 + 200 OK 时生成的 To-tag）
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
    // 设置 From tag（我们生成的 To-tag）
    if (!ctx.upstreamToTag.empty()) {
      pj_str_t tag_str = pj_str((char*)ctx.upstreamToTag.c_str());
      pj_strdup(pool, &from_hdr->tag, &tag_str);
    }
  }

  // 创建 To 头（使用上级 From 的信息）
  pjsip_to_hdr* to_hdr = pjsip_to_hdr_create(pool);
  if (to_hdr) {
    // 解析上级 From URI
    std::string remoteUri = ctx.upstreamFromUri;
    if (remoteUri.find("sip:") != 0) {
      remoteUri = "sip:" + remoteUri;
    }
    pj_str_t remoteUriPj = pj_str((char*)remoteUri.c_str());
    pjsip_uri* remote_uri = pjsip_parse_uri(pool, remoteUriPj.ptr, remoteUriPj.slen, PJSIP_PARSE_URI_AS_NAMEADDR);
    if (remote_uri) {
      to_hdr->uri = remote_uri;
    }
    // 设置 To tag（上级 From 的 tag）
    if (!ctx.upstreamFromTag.empty()) {
      pj_str_t tag_str = pj_str((char*)ctx.upstreamFromTag.c_str());
      pj_strdup(pool, &to_hdr->tag, &tag_str);
    }
  }

  // 创建 BYE 请求
  pjsip_method method;
  pjsip_method_set(&method, PJSIP_BYE_METHOD);

  pjsip_tx_data* tdata = nullptr;
  pj_status_t st = pjsip_endpt_create_request_from_hdr(g_sip_endpt,
                                                        &method,
                                                        target_uri,
                                                        from_hdr,
                                                        to_hdr,
                                                        nullptr,  // Contact
                                                        nullptr,  // Call-ID (will set later)
                                                        -1,       // CSeq
                                                        nullptr,  // body
                                                        &tdata);
  if (st != PJ_SUCCESS || !tdata) {
    ErrorL << "【sendUpstreamPlatformBye】Failed to create request: " << st;
    pjsip_endpt_release_pool(g_sip_endpt, pool);
    return false;
  }

  // 设置 Call-ID
  pjsip_cid_hdr* call_id_hdr = (pjsip_cid_hdr*)pjsip_msg_find_hdr(tdata->msg, PJSIP_H_CALL_ID, nullptr);
  if (call_id_hdr) {
    pj_strdup2(pool, &call_id_hdr->id, ctx.upstreamCallId.c_str());
  }

  // 设置 Via 头
  static int byeUpstreamCounter = 0;
  byeUpstreamCounter++;
  pjsip_via_hdr* via = (pjsip_via_hdr*)pjsip_msg_find_hdr(tdata->msg, PJSIP_H_VIA, nullptr);
  if (via) {
    pj_strdup2(pool, &via->transport, (char*)"UDP");
    pj_strdup2(pool, &via->sent_by.host, (char*)localHost.c_str());
    via->sent_by.port = localPort;
    char branch_buf[64];
    snprintf(branch_buf, sizeof(branch_buf), "z9hG4bKupbye%d", byeUpstreamCounter);
    pj_strdup2(pool, &via->branch_param, branch_buf);
  }

  // 设置 UDP 目的地址（上级信令源）
  if (!ctx.upstreamSignalIp.empty() && ctx.upstreamSignalPort > 0) {
    pj_sockaddr_in addr;
    pj_str_t addrStr = pj_str((char*)ctx.upstreamSignalIp.c_str());
    pj_sockaddr_in_init(&addr, &addrStr, ctx.upstreamSignalPort);
    tdata->dest_info.addr.count = 1;
    tdata->dest_info.addr.entry[0].type = PJSIP_TRANSPORT_UDP;
    tdata->dest_info.addr.entry[0].addr_len = sizeof(addr);
    pj_memcpy(&tdata->dest_info.addr.entry[0].addr, &addr, sizeof(addr));
  }

  // 打印 SIP 消息
  char msg_buf[2048];
  pj_ssize_t msg_len = pjsip_msg_print(tdata->msg, msg_buf, sizeof(msg_buf));
  if (msg_len > 0) {
    InfoL << "【sendUpstreamPlatformBye】SIP BYE 消息:\n" << std::string(msg_buf, msg_len);
  }

  // 发送 BYE 请求
  st = pjsip_endpt_send_request_stateless(g_sip_endpt, tdata, nullptr, nullptr);
  if (st != PJ_SUCCESS) {
    ErrorL << "【sendUpstreamPlatformBye】Failed to send BYE: " << st;
    pjsip_endpt_release_pool(g_sip_endpt, pool);
    return false;
  }

  InfoL << "【sendUpstreamPlatformBye】BYE sent successfully for call " << ctx.upstreamCallId;
  pjsip_endpt_release_pool(g_sip_endpt, pool);
  return true;
}

void upstreamBridgeRecordDeviceInviteSsrc(const std::string& deviceCallId, const std::string& ssrc) {
  std::lock_guard<std::mutex> lock(g_mtx);
  auto it = g_byDeviceCallId.find(deviceCallId);
  if (it == g_byDeviceCallId.end()) return;
  auto b = it->second.lock();
  if (!b) return;
  b->ssrc = ssrc;
}

void upstreamBridgeOnDeviceInviteSendFailed(const std::string& deviceCallId) {
  std::shared_ptr<UpstreamBridgeCtx> b;
  {
    std::lock_guard<std::mutex> lock(g_mtx);
    auto it = g_byDeviceCallId.find(deviceCallId);
    if (it == g_byDeviceCallId.end()) return;
    b = it->second.lock();
    if (!b) {
      g_byDeviceCallId.erase(it);
      return;
    }
  }
  teardownBridge(b, "device_invite_send_failed");
}

bool tryHandleUpstreamPlatformInvite(pjsip_rx_data* rdata, const UpstreamSignalMatch& um) {
  if (!rdata || !rdata->msg_info.msg || !g_sip_endpt) return false;
  pjsip_msg* msg = rdata->msg_info.msg;

  sendSimpleResp(rdata, 100);

  std::string sdpIn;
  if (msg->body && msg->body->data && msg->body->len > 0) {
    sdpIn.assign((const char*)msg->body->data, (size_t)msg->body->len);
  }
  std::string dstIp;
  uint16_t dstPort = 0;
  bool offerTcp = false;
  if (!parseOfferVideoDest(sdpIn, dstIp, dstPort, offerTcp)) {
    InfoL << "【UpstreamInviteBridge】无法解析上级 SDP 媒体地址";
    sendSimpleResp(rdata, 400);
    return true;
  }
  InfoL << "【UpstreamInviteBridge】上级媒体邀约 " << (offerTcp ? "TCP" : "UDP") << "（答包/ZLM startSendRtp 将一致；下级传输仍按平台策略）";

  std::string subj = headerValueByName(msg, "Subject");
  std::string catalogId = parseSubjectChannel(subj);
  if (catalogId.empty()) {
    pjsip_to_hdr* toh = (pjsip_to_hdr*)pjsip_msg_find_hdr(msg, PJSIP_H_TO, nullptr);
    if (toh && toh->uri) {
      char buf[256];
      int n = pjsip_uri_print(PJSIP_URI_IN_FROMTO_HDR, toh->uri, buf, sizeof(buf) - 1);
      if (n > 0) {
        buf[n] = '\0';
        std::string uri(buf);
        size_t sip = uri.find("sip:");
        if (sip != std::string::npos) {
          size_t at = uri.find('@', sip);
          if (at != std::string::npos) catalogId = trimField(uri.substr(sip + 4, at - (sip + 4)));
        }
      }
    }
  }
  if (catalogId.empty()) {
    InfoL << "【UpstreamInviteBridge】无通道 ID（Subject/To）";
    sendSimpleResp(rdata, 400);
    return true;
  }

  std::string devGb, platGb;
  long long camDb = 0;
  if (!resolveUpstreamCatalogDeviceId(um.platformDbId, catalogId, devGb, platGb, camDb) || devGb.empty() || platGb.empty()) {
    InfoL << "【UpstreamInviteBridge】ID 无映射 catalogId=" << catalogId << " upstreamDb=" << um.platformDbId;
    sendSimpleResp(rdata, 403);
    return true;
  }
  InfoL << "【UpstreamInviteBridge】ID 映射 catalogId=" << catalogId << " → devGb=" << devGb
        << " platGb=" << platGb << " camDb=" << camDb;

  uint64_t t0 = 0, t1 = 0;
  parsePlaybackUnix(sdpIn, t0, t1);
  bool isDl = (sdpIn.find("s=Download") != std::string::npos);
  const bool replayBySdp = isDl || (sdpIn.find("s=Playback") != std::string::npos) || (t0 > 0 && t1 > 0);
  const bool replayByHint = consumeReplayHint(um.platformDbId, catalogId);
  const bool isReplayInvite = replayBySdp || replayByHint;
  InfoL << "【UpstreamInviteBridge】回放判定 replayBySdp=" << replayBySdp << " replayByHint=" << replayByHint
        << " final=" << isReplayInvite << " t0=" << t0 << " t1=" << t1;

  auto& media = MediaService::instance();
  // 与 HTTP 预览一致：streamId = platformGbId + "_" + channelGbId；判断“已有流”需 ZLM + 本级会话（仅 getMediaList 会漏检）
  std::string expectedStreamId = platGb + "_" + devGb;
  const bool zlmListed = media.isStreamExistsInZlm(expectedStreamId);
  const bool localPlaying = media.isLocalStreamPlaying(expectedStreamId);
  const bool streamAlreadyExists = zlmListed || localPlaying;
  InfoL << "【UpstreamInviteBridge】流检查 streamId=" << expectedStreamId << " zlmListed=" << zlmListed
        << " localStreaming=" << localPlaying << " treatAsExists=" << streamAlreadyExists;

  if (streamAlreadyExists && !isReplayInvite) {
    switch (tryAnswerUpstreamWithExistingZlm(rdata, msg, media, catalogId, expectedStreamId, platGb, devGb, dstIp,
                                           dstPort, offerTcp)) {
      case UpstreamDirectAnswer::Ok:
        return true;
      case UpstreamDirectAnswer::BadRequest:
        sendSimpleResp(rdata, 400);
        return true;
      case UpstreamDirectAnswer::StartSendRtpFailed:
        InfoL << "【UpstreamInviteBridge】流存在但 startSendRtp 失败，回退到向下级要流";
        goto need_pull_from_device;
    }
  }

need_pull_from_device:
  std::string sdpConnIp;
  bool downTcp = false;
  resolveSdpConnForPlatform(platGb, sdpConnIp, downTcp);

  pjsip_rx_data* upClone = nullptr;
  if (pjsip_rx_data_clone(rdata, 0, &upClone) != PJ_SUCCESS || !upClone) {
    sendSimpleResp(rdata, 500);
    return true;
  }

  pjsip_cid_hdr* ucid = (pjsip_cid_hdr*)pjsip_msg_find_hdr(msg, PJSIP_H_CALL_ID, nullptr);
  if (!ucid || ucid->id.slen <= 0) {
    pjsip_rx_data_free_cloned(upClone);
    sendSimpleResp(rdata, 400);
    return true;
  }
  std::string upstreamCallId(ucid->id.ptr, (size_t)ucid->id.slen);

  auto session = media.createSession(devGb, devGb, platGb, std::to_string(camDb));
  if (!session) {
    pjsip_rx_data_free_cloned(upClone);
    sendSimpleResp(rdata, 500);
    return true;
  }
  // 预览等已为本 stream 调用过 openRtpServer 时，createSession 返回同一会话且 zlmPort 已分配；
  // 再 openRtpServer 会因 ZLM 拒绝同 stream_id 重复注册而失败。先尝试直推上级；若 ZLM 无源导致失败，
  // 则关闭陈旧会话并继续 openRtpServer + 下级 INVITE（勿在此处 free upClone，除非已结束或直推成功）。
  if (session->zlmPort != 0) {
    InfoL << "【UpstreamInviteBridge】同 stream 已有本地会话，尝试直推上级 session=" << session->sessionId
          << " zlmPort=" << session->zlmPort << " status=" << static_cast<int>(session->status);
    if (!isReplayInvite) switch (tryAnswerUpstreamWithExistingZlm(rdata, msg, media, catalogId, session->streamId, platGb, devGb, dstIp,
                                             dstPort, offerTcp)) {
      case UpstreamDirectAnswer::Ok:
        pjsip_rx_data_free_cloned(upClone);
        return true;
      case UpstreamDirectAnswer::BadRequest:
        pjsip_rx_data_free_cloned(upClone);
        sendSimpleResp(rdata, 400);
        return true;
      case UpstreamDirectAnswer::StartSendRtpFailed:
        WarnL << "【UpstreamInviteBridge】复用会话直推失败(ZLM 无源/陈旧会话)，关闭会话后重新 openRtp+下级 INVITE";
        media.closeSession(session->sessionId);
        session = media.createSession(devGb, devGb, platGb, std::to_string(camDb));
        if (!session) {
          pjsip_rx_data_free_cloned(upClone);
          sendSimpleResp(rdata, 500);
          return true;
        }
        if (session->zlmPort != 0) {
          WarnL << "【UpstreamInviteBridge】重建会话仍带 zlmPort，异常";
          pjsip_rx_data_free_cloned(upClone);
          sendSimpleResp(rdata, 503);
          return true;
        }
        break;
    }
    if (isReplayInvite) {
      InfoL << "【UpstreamInviteBridge】回放 INVITE 命中，禁止复用已有直播流，重建会话后向下级拉回放";
      media.closeSession(session->sessionId);
      session = media.createSession(devGb, devGb, platGb, std::to_string(camDb));
      if (!session) {
        pjsip_rx_data_free_cloned(upClone);
        sendSimpleResp(rdata, 500);
        return true;
      }
      if (session->zlmPort != 0) {
        WarnL << "【UpstreamInviteBridge】回放重建会话仍带 zlmPort，异常";
        pjsip_rx_data_free_cloned(upClone);
        sendSimpleResp(rdata, 503);
        return true;
      }
    }
  }
  uint16_t zlmPort = 0;
  int zlmTcp = downTcp ? 1 : 0;
  if (!media.openRtpServer(session->streamId, zlmPort, zlmTcp)) {
    WarnL << "【UpstreamInviteBridge】openRtpServer 失败 → 503，请检查 ZLM 是否运行、"
             "api 地址/secret、本机能否访问 ZLM。streamId=" << session->streamId
          << " tcp_mode=" << zlmTcp;
    media.closeSession(session->sessionId);
    pjsip_rx_data_free_cloned(upClone);
    sendSimpleResp(rdata, 503);
    return true;
  }
  session->zlmPort = zlmPort;
  media.setSessionCallId(session->sessionId, "");
  media.updateSessionStatus(session->sessionId, StreamSessionStatus::INVITING);

  static std::atomic<int> seq{0};
  std::string localHost = "127.0.0.1";
  {
    std::string out = execPsql("SELECT COALESCE(signal_ip, '127.0.0.1') FROM gb_local_config LIMIT 1");
    if (!out.empty()) {
      size_t nl = out.find('\n');
      if (nl != std::string::npos) out = out.substr(0, nl);
      std::string t = trimField(out);
      if (!t.empty()) localHost = t;
    }
  }
  std::string deviceCallId =
      "upcv-" + std::to_string(++seq) + "-" + std::to_string(um.platformDbId) + "@" + localHost;

  auto ctx = std::make_shared<UpstreamBridgeCtx>();
  ctx->upstreamCallId = upstreamCallId;
  ctx->upstreamInviteClone = upClone;
  ctx->deviceCallId = deviceCallId;
  ctx->streamId = session->streamId;
  ctx->sessionId = session->sessionId;
  ctx->platformGbId = platGb;
  ctx->channelGbId = devGb;
  ctx->catalogGbId = catalogId;
  ctx->zlmPort = zlmPort;
  ctx->sendDstIp = dstIp;
  ctx->sendDstPort = dstPort;
  ctx->offerTcp = offerTcp;

  {
    std::lock_guard<std::mutex> lock(g_mtx);
    g_byUpstreamCallId[upstreamCallId] = ctx;
    g_byDeviceCallId[deviceCallId] = ctx;
  }

  media.setSessionCallId(session->sessionId, deviceCallId);

  if (!enqueuePlayInviteWithCallId(platGb, devGb, zlmPort, deviceCallId, sdpConnIp, downTcp, t0, t1, isDl)) {
    teardownBridge(ctx, "enqueue_invite_failed");
    sendSimpleResp(rdata, 500);
    return true;
  }

  InfoL << "【UpstreamInviteBridge】流不存在，向下级要流 上级Call-ID=" << upstreamCallId
        << " 下级Call-ID=" << deviceCallId << " stream=" << session->streamId
        << " catalogId=" << catalogId << " zlmPort=" << zlmPort;
  return true;
}

/** 源流已在 ZLM：startSendRtp 并 200 答上级 */
static bool completeUpstreamRtpToSuperior(const std::shared_ptr<UpstreamBridgeCtx>& b) {
  if (!b || !b->upstreamInviteClone) return false;
  uint16_t localPort = 0;
  auto& media = MediaService::instance();
  const bool zlmUdpToUpstream = !b->offerTcp;
  if (!media.startSendRtpPs(b->streamId, b->ssrc, b->sendDstIp, b->sendDstPort, zlmUdpToUpstream, localPort)) {
    InfoL << "【UpstreamInviteBridge】startSendRtp 失败 stream=" << b->streamId;
    return false;
  }

  std::string mediaIp;
  bool unusedTcp = false;
  resolveSdpConnForPlatform(b->platformGbId, mediaIp, unusedTcp);

  pjsip_rx_data* up = b->upstreamInviteClone;
  std::ostringstream sdp;
  sdp << "v=0\r\n";
  sdp << "o=" << b->catalogGbId << " 0 0 IN IP4 " << mediaIp << "\r\n";
  sdp << "s=Play\r\n";
  sdp << "c=IN IP4 " << mediaIp << "\r\n";
  sdp << "t=0 0\r\n";
  if (b->offerTcp) {
    sdp << "m=video " << localPort << " TCP/RTP/AVP 96\r\n";
    sdp << "a=setup:passive\r\n";
    sdp << "a=connection:new\r\n";
  } else {
    sdp << "m=video " << localPort << " RTP/AVP 96\r\n";
  }
  sdp << "a=rtpmap:96 PS/90000\r\n";
  sdp << "a=sendonly\r\n";
  sdp << "y=" << b->ssrc << "\r\n";

  // 发送 200 OK 并获取 To-tag，同时提取 SIP 对话字段
  std::string toTag = sendSdpAnswer(up, sdp.str());
  b->upstreamToTag = toTag;

  // 从原始 INVITE 提取 SIP 对话字段
  pjsip_msg* msg = up->msg_info.msg;
  extractUpstreamSipDialog(up, msg, *b);

  pjsip_rx_data_free_cloned(up);
  b->upstreamInviteClone = nullptr;
  b->answeredUpstream = true;
  media.updateSessionStatus(b->sessionId, StreamSessionStatus::STREAMING);
  InfoL << "【UpstreamInviteBridge】已答上级 200 OK 并 startSendRtp local_port=" << localPort << " -> " << b->sendDstIp
        << ":" << b->sendDstPort << " upstream_media=" << (b->offerTcp ? "TCP" : "UDP");
  return true;
}

bool upstreamBridgeTryTeardownForRtpServerTimeout(const std::string& streamId) {
  if (streamId.empty()) return false;
  std::shared_ptr<UpstreamBridgeCtx> b;
  {
    std::lock_guard<std::mutex> lock(g_mtx);
    for (const auto& kv : g_byUpstreamCallId) {
      if (kv.second && kv.second->streamId == streamId) {
        b = kv.second;
        break;
      }
    }
  }
  if (!b) return false;
  teardownBridge(b, "zlm_on_rtp_server_timeout");
  return true;
}

void processUpstreamZlmSourceWaitPoll() {
  std::vector<std::shared_ptr<UpstreamBridgeCtx>> polling;
  {
    std::lock_guard<std::mutex> lock(g_mtx);
    polling.reserve(g_byUpstreamCallId.size());
    for (const auto& kv : g_byUpstreamCallId) {
      if (kv.second && kv.second->waitingZlmSource) {
        polling.push_back(kv.second);
      }
    }
  }
  if (polling.empty()) return;

  auto& media = MediaService::instance();
  const auto now = std::chrono::steady_clock::now();

  for (const auto& b : polling) {
    if (!b || !b->waitingZlmSource) continue;
    if (now >= b->zlmSourceWaitUntil) {
      InfoL << "【UpstreamInviteBridge】等待 ZLM 源流本地超时(后备，权威为 ZLM on_rtp_server_timeout) streamId="
            << b->streamId;
      teardownBridge(b, "zlm_source_wait_timeout");
      continue;
    }
    if (!media.isStreamExistsInZlm(b->streamId)) continue;

    b->waitingZlmSource = false;
    if (!completeUpstreamRtpToSuperior(b)) {
      teardownBridge(b, "start_send_rtp_failed");
    }
  }
}

void upstreamBridgeOnDeviceInviteRxResponse(pjsip_rx_data* rdata, int statusCode) {
  pjsip_cid_hdr* cid = (pjsip_cid_hdr*)pjsip_msg_find_hdr(rdata->msg_info.msg, PJSIP_H_CALL_ID, nullptr);
  if (!cid || cid->id.slen <= 0) return;
  std::string dcid(cid->id.ptr, (size_t)cid->id.slen);

  std::shared_ptr<UpstreamBridgeCtx> b;
  {
    std::lock_guard<std::mutex> lock(g_mtx);
    auto it = g_byDeviceCallId.find(dcid);
    if (it == g_byDeviceCallId.end()) return;
    b = it->second.lock();
  }
  if (!b) return;

  if (statusCode != 200) {
    InfoL << "【UpstreamInviteBridge】下级 INVITE 失败 SIP status=" << statusCode << " Call-ID=" << dcid;
    teardownBridge(b, "device_non_200");
    return;
  }

  if (!b->upstreamInviteClone) {
    InfoL << "【UpstreamInviteBridge】上级 INVITE 克隆已释放，忽略重复 200 Call-ID=" << dcid;
    return;
  }

  std::string sdpBody;
  pjsip_msg* msg = rdata->msg_info.msg;
  if (msg->body && msg->body->data && msg->body->len > 0) {
    sdpBody.assign((const char*)msg->body->data, (size_t)msg->body->len);
  }
  std::string yssrc = extractYssrc(sdpBody);
  if (yssrc.empty()) yssrc = b->ssrc;
  if (yssrc.empty()) {
    InfoL << "【UpstreamInviteBridge】无 SSRC，无法 startSendRtp";
    teardownBridge(b, "no_ssrc");
    return;
  }
  b->ssrc = yssrc;

  auto& media = MediaService::instance();
  const int waitSec = media.getZlmOpenRtpServerWaitSec();

  if (media.isStreamExistsInZlm(b->streamId)) {
    if (!completeUpstreamRtpToSuperior(b)) {
      teardownBridge(b, "start_send_rtp_failed");
    }
    return;
  }

  b->waitingZlmSource = true;
  b->zlmSourceWaitUntil = std::chrono::steady_clock::now() + std::chrono::seconds(waitSec);
  InfoL << "【UpstreamInviteBridge】下级已 200，轮询 getMediaList 直至源流就绪；无 RTP 时由 ZLM on_rtp_server_timeout "
           "回调 teardown，本地最长约 "
        << waitSec << "s(后备) streamId=" << b->streamId;
}

bool tryHandleUpstreamBye(pjsip_rx_data* rdata, const UpstreamSignalMatch& um) {
  (void)um;
  if (!rdata || !rdata->msg_info.msg) return false;
  pjsip_cid_hdr* cid = (pjsip_cid_hdr*)pjsip_msg_find_hdr(rdata->msg_info.msg, PJSIP_H_CALL_ID, nullptr);
  if (!cid || cid->id.slen <= 0) return false;
  std::string upCid(cid->id.ptr, (size_t)cid->id.slen);
  std::shared_ptr<UpstreamBridgeCtx> b;
  {
    std::lock_guard<std::mutex> lock(g_mtx);
    auto it = g_byUpstreamCallId.find(upCid);
    if (it == g_byUpstreamCallId.end()) return false;
    b = it->second;
  }
  teardownBridge(b, "upstream_bye");
  sendSimpleResp(rdata, 200);
  return true;
}

bool tryHandleUpstreamCancel(pjsip_rx_data* rdata, const UpstreamSignalMatch& um) {
  (void)um;
  if (!rdata || !rdata->msg_info.msg) return false;
  pjsip_cid_hdr* cid = (pjsip_cid_hdr*)pjsip_msg_find_hdr(rdata->msg_info.msg, PJSIP_H_CALL_ID, nullptr);
  if (!cid || cid->id.slen <= 0) return false;
  std::string upCid(cid->id.ptr, (size_t)cid->id.slen);
  std::shared_ptr<UpstreamBridgeCtx> b;
  {
    std::lock_guard<std::mutex> lock(g_mtx);
    auto it = g_byUpstreamCallId.find(upCid);
    if (it == g_byUpstreamCallId.end()) return false;
    b = it->second;
  }
  teardownBridge(b, "upstream_cancel");
  sendSimpleResp(rdata, 200);
  return true;
}

/** ZLM Hook on_send_rtp_stopped：上级断流时触发 */
void upstreamBridgeOnZlmSendRtpStopped(const std::string& streamId, const std::string& ssrc) {
  if (streamId.empty()) {
    WarnL << "【upstreamBridgeOnZlmSendRtpStopped】streamId 为空，忽略";
    return;
  }

  // 查找对应桥接
  std::shared_ptr<UpstreamBridgeCtx> ctx;
  {
    std::lock_guard<std::mutex> lock(g_mtx);
    for (const auto& kv : g_byUpstreamCallId) {
      if (kv.second && kv.second->streamId == streamId) {
        ctx = kv.second;
        break;
      }
    }
  }

  if (!ctx) {
    InfoL << "【upstreamBridgeOnZlmSendRtpStopped】未找到对应桥接，可能已 teardown stream=" << streamId;
    return;
  }

  InfoL << "【上级断流】on_send_rtp_stopped stream=" << streamId
        << " ssrc=" << ssrc
        << " upstreamCallId=" << ctx->upstreamCallId;

  // 从 map 中移除桥接（避免重复处理）
  {
    std::lock_guard<std::mutex> lock(g_mtx);
    eraseBridgeLocked(ctx);
  }

  auto& media = MediaService::instance();

  // 1. 停止向该 SSRC 的推流（ZLM startSendRtp）
  if (!ctx->ssrc.empty()) {
    media.stopSendRtpForStream(ctx->streamId, ctx->ssrc);
    InfoL << "【upstreamBridgeOnZlmSendRtpStopped】已停止推流 stream=" << streamId
          << " ssrc=" << ctx->ssrc;
  }

  // 2. 向上级发送 BYE（如果已应答上级）
  if (ctx->answeredUpstream && !ctx->upstreamCallId.empty()) {
    sendUpstreamPlatformBye(*ctx);
  }

  // 3. 向下级发送 BYE 并清理会话
  if (!ctx->sessionId.empty()) {
    if (!ctx->deviceCallId.empty()) {
      sendPlayByeAsync(ctx->deviceCallId, ctx->platformGbId, ctx->channelGbId);
      InfoL << "【upstreamBridgeOnZlmSendRtpStopped】已向下级发送 BYE callId=" << ctx->deviceCallId;
    }
    media.closeSession(ctx->sessionId);
  } else if (ctx->zlmPort > 0 && !ctx->streamId.empty()) {
    // 只有纯推流（无下级会话）时关闭 RTP server
    media.closeRtpServer(ctx->streamId);
  }

  InfoL << "【upstreamBridgeOnZlmSendRtpStopped】处理完成 stream=" << streamId;
}

void upstreamBridgeMarkReplayHint(int64_t upstreamDbId, const std::string& catalogGbId, int ttlSec) {
  if (upstreamDbId <= 0 || catalogGbId.empty()) return;
  if (ttlSec <= 0) ttlSec = 45;
  auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lk(g_replay_hint_mu);
  cleanupReplayHintsLocked(now);
  g_replay_hints[makeReplayHintKey(upstreamDbId, catalogGbId)] = now + std::chrono::seconds(ttlSec);
  InfoL << "【UpstreamInviteBridge】已标记回放关联窗口 upstreamId=" << upstreamDbId
        << " catalogId=" << catalogGbId << " ttl=" << ttlSec << "s";
}

}  // namespace gb
