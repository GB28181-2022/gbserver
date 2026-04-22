/**
 * @file UpstreamRegistrar.cpp
 * @brief 上级平台 UAC REGISTER（pjsip_regc）、MESSAGE 保活、路由缓存
 */
#include "infra/UpstreamRegistrar.h"
#include "infra/DbUtil.h"
#include "infra/AuthHelper.h"
#include "infra/SipServerPjsip.h"
#include "infra/SipCatalog.h"
#include "infra/UpstreamPlatformService.h"
#include "Util/logger.h"
#include <pjsip.h>
#include <pjsip/sip_msg.h>
#include <pjsip/sip_transaction.h>
#include <pjsip-ua/sip_regc.h>
#include <pjlib.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <unordered_map>
#include <vector>

using namespace toolkit;

namespace gb {

extern pjsip_endpoint* g_sip_endpt;

namespace {

/** 全量重载（清空增量队列） */
std::atomic<bool> g_full_reload_requested{false};
/** 单条同步队列：'U'=Upsert, 'D'=Delete */
std::mutex g_inc_mu;
std::vector<std::pair<char, int64_t>> g_inc_ops;

std::mutex g_mu;
std::mutex g_notify_mu;
std::vector<int64_t> g_pending_catalog_notify;

struct PendingQueryResponse {
  int64_t upstreamDbId;
  int sn;
};
static std::mutex g_query_rsp_mu;
static std::vector<PendingQueryResponse> g_pending_query_rsp;

enum class RetryTaskType { CatalogNotify, CatalogQueryResponse };

struct RetryTask {
  RetryTaskType type{RetryTaskType::CatalogNotify};
  int64_t upstreamDbId{0};
  std::string sip_ip;
  int sip_port{5060};
  std::string gb_id;
  std::string method;
  std::string body;
  int attempt{0};
  int max_retries{0};
  int base_delay_sec{2};
  int max_delay_sec{300};
  double jitter_ratio{0.2};
  std::chrono::steady_clock::time_point next_attempt{};
  std::chrono::steady_clock::time_point deadline{};
};
static std::mutex g_retry_mu;
static std::vector<RetryTask> g_retry_tasks;

static constexpr int kKeepaliveFailThreshold = 3;
static constexpr int kRegisterRetryMaxDelaySec = 60;
static constexpr int kRegisterBurstWindowSec = 10;
static constexpr int kRegisterBurstWarnThreshold = 5;

struct CachedRoute {
  int64_t id{0};
  std::string gb_id;
  std::string sip_ip;
  int sip_port{5060};
  bool enabled{true};
};

std::vector<CachedRoute> g_routes;

struct RegEntry {
  int64_t db_id{0};
  pjsip_regc* regc{nullptr};
  std::string sip_ip;
  int sip_port{5060};
  std::string upstream_gb_id;
  std::string sip_domain;
  std::string reg_username;
  std::string reg_password;
  int heartbeat_interval{60};
  bool enabled{true};
  std::chrono::steady_clock::time_point next_keepalive{};
  bool registration_ok{false};
  bool register_inflight{false};
  int keepalive_failures{0};
  int register_retry_attempt{0};
  std::chrono::steady_clock::time_point next_register_retry{};
  int register_burst_count{0};
  std::chrono::steady_clock::time_point register_burst_window_start{};
};

std::vector<std::unique_ptr<RegEntry>> g_regs;
std::mutex g_reg_retry_mu;
std::unordered_map<int64_t, int> g_reg_retry_attempt_by_id;

/**
 * @brief 将 Catalog Response XML 的 <SN> 强制改为指定值
 * @details 目录检索应答分包应复用“上级查询的同一个 SN”，
 *          避免接收端按 (DeviceID,SN) 聚合时被拆成多条不完整会话。
 */
static void forceCatalogResponseSn(std::string& xml, int fixedSn) {
  const std::string openTag = "<SN>";
  const std::string closeTag = "</SN>";
  size_t s = xml.find(openTag);
  if (s == std::string::npos) return;
  s += openTag.size();
  size_t e = xml.find(closeTag, s);
  if (e == std::string::npos || e < s) return;
  xml.replace(s, e - s, std::to_string(fixedSn));
}

/** sip:user@host:port，IPv6 的 host 需方括号 */
static std::string sipHostPortLiteral(const std::string& ip, int port) {
  if (ip.empty()) return "";
  bool v6 = (ip.find(':') != std::string::npos);
  if (v6 && (ip.empty() || ip.front() != '['))
    return "[" + ip + "]:" + std::to_string(port);
  return ip + ":" + std::to_string(port);
}

static std::string trimSql(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

static void splitLines(const std::string& out, std::vector<std::string>& lines) {
  lines.clear();
  size_t pos = 0;
  while (pos < out.size()) {
    size_t nl = out.find('\n', pos);
    std::string line = (nl == std::string::npos) ? out.substr(pos) : out.substr(pos, nl - pos);
    if (!trimSql(line).empty()) lines.push_back(line);
    if (nl == std::string::npos) break;
    pos = nl + 1;
  }
}

static std::vector<std::string> splitPipe(const std::string& line) {
  std::vector<std::string> cols;
  std::string cur;
  for (char c : line) {
    if (c == '|') {
      cols.push_back(cur);
      cur.clear();
    } else
      cur += c;
  }
  cols.push_back(cur);
  return cols;
}

static void updateUpstreamOnline(int64_t id, bool online) {
  std::string sql =
      std::string("UPDATE upstream_platforms SET online=") + (online ? "true" : "false") +
      ", last_heartbeat_at=CURRENT_TIMESTAMP, updated_at=CURRENT_TIMESTAMP WHERE id=" + std::to_string(id);
  execPsqlCommand(sql);
}

static int computeRegisterRetryDelaySec(int attempt) {
  int exp = std::max(0, std::min(attempt, 4));
  int delay = 10 * (1 << exp);  // 10,20,40,80...
  return std::min(delay, kRegisterRetryMaxDelaySec);
}

static int getRegisterRetryAttempt(int64_t id) {
  std::lock_guard<std::mutex> lk(g_reg_retry_mu);
  auto it = g_reg_retry_attempt_by_id.find(id);
  return it == g_reg_retry_attempt_by_id.end() ? 0 : std::max(0, it->second);
}

static void setRegisterRetryAttempt(int64_t id, int attempt) {
  std::lock_guard<std::mutex> lk(g_reg_retry_mu);
  g_reg_retry_attempt_by_id[id] = std::max(0, attempt);
}

static void clearRegisterRetryAttempt(int64_t id) {
  std::lock_guard<std::mutex> lk(g_reg_retry_mu);
  g_reg_retry_attempt_by_id.erase(id);
}

static int computeRetryDelaySec(const RetryTask& task, int nextAttempt) {
  int exp = std::max(0, std::min(nextAttempt - 1, 10));
  int base = std::max(1, task.base_delay_sec);
  int delay = base * (1 << exp);
  delay = std::min(delay, std::max(1, task.max_delay_sec));
  if (task.jitter_ratio <= 0.0) return delay;
  static std::mt19937 rng{std::random_device{}()};
  std::uniform_real_distribution<double> dist(1.0 - task.jitter_ratio, 1.0 + task.jitter_ratio);
  int jittered = static_cast<int>(std::round(static_cast<double>(delay) * dist(rng)));
  return std::max(1, jittered);
}

static void enqueueRetryTask(RetryTask&& task) {
  std::lock_guard<std::mutex> lk(g_retry_mu);
  g_retry_tasks.emplace_back(std::move(task));
}

static void markRegisterBurst(RegEntry& entry) {
  auto now = std::chrono::steady_clock::now();
  if (entry.register_burst_window_start.time_since_epoch().count() == 0 ||
      now - entry.register_burst_window_start > std::chrono::seconds(kRegisterBurstWindowSec)) {
    entry.register_burst_window_start = now;
    entry.register_burst_count = 1;
    return;
  }
  entry.register_burst_count++;
  if (entry.register_burst_count >= kRegisterBurstWarnThreshold) {
    WarnL << "【上级REGISTER】短时间重注册偏高 id=" << entry.db_id << " window=" << kRegisterBurstWindowSec
          << "s count=" << entry.register_burst_count
          << "（请检查是否存在循环触发重建/网络抖动）";
  }
}

static void on_regc_cb(struct pjsip_regc_cbparam* param) {
  if (!param) return;
  auto* entry = static_cast<RegEntry*>(param->token);
  if (!entry) return;
  entry->register_inflight = false;
  if (param->status != PJ_SUCCESS) {
    WarnL << "【上级REGISTER】id=" << entry->db_id << " 错误 status=" << param->status;
    entry->registration_ok = false;
    entry->keepalive_failures = 0;
    updateUpstreamOnline(entry->db_id, false);
    int delay = computeRegisterRetryDelaySec(entry->register_retry_attempt++);
    setRegisterRetryAttempt(entry->db_id, entry->register_retry_attempt);
    entry->next_register_retry = std::chrono::steady_clock::now() + std::chrono::seconds(delay);
    return;
  }
  if (param->code >= 200 && param->code < 300) {
    entry->registration_ok = true;
    entry->keepalive_failures = 0;
    entry->register_retry_attempt = 0;
    clearRegisterRetryAttempt(entry->db_id);
    entry->next_register_retry = std::chrono::steady_clock::time_point{};
    updateUpstreamOnline(entry->db_id, true);
    entry->next_keepalive = std::chrono::steady_clock::now();
    InfoL << "【上级REGISTER】成功 id=" << entry->db_id << " code=" << param->code;
  } else {
    entry->registration_ok = false;
    entry->keepalive_failures = 0;
    updateUpstreamOnline(entry->db_id, false);
    int delay = computeRegisterRetryDelaySec(entry->register_retry_attempt++);
    setRegisterRetryAttempt(entry->db_id, entry->register_retry_attempt);
    entry->next_register_retry = std::chrono::steady_clock::now() + std::chrono::seconds(delay);
    WarnL << "【上级REGISTER】失败 id=" << entry->db_id << " code=" << param->code;
  }
}

/**
 * @brief 释放 regc；若曾发起过注册，先发 REGISTER Expires=0 再销毁（国标/对接侧期望显式注销）
 */
static void destroyRegcHandle(pjsip_regc* regc, int64_t idForLog, bool sendUnregister = true) {
  if (!regc) return;
  if (sendUnregister) {
    pjsip_tx_data* tdata = nullptr;
    pj_status_t st = pjsip_regc_unregister(regc, &tdata);
    if (st == PJ_SUCCESS && tdata) {
      st = pjsip_regc_send(regc, tdata);
      if (st == PJ_SUCCESS) {
        InfoL << "【上级REGISTER】已发送注销(Expires=0) id=" << idForLog;
      } else {
        WarnL << "【上级REGISTER】注销 REGISTER 发送失败 id=" << idForLog << " st=" << st;
      }
    } else if (st != PJ_SUCCESS) {
      DebugL << "【上级REGISTER】pjsip_regc_unregister 未构建请求 id=" << idForLog << " st=" << st
             << "（可能尚未成功注册，仍释放 regc）";
    }
  }
  pjsip_regc_destroy(regc);
}

static void destroyAllRegc() {
  for (auto& e : g_regs) {
    if (e && e->regc) {
      destroyRegcHandle(e->regc, e->db_id);
      e->regc = nullptr;
    }
  }
  g_regs.clear();
}

static void destroyRegcForId(int64_t id, bool sendUnregister = true) {
  for (auto it = g_regs.begin(); it != g_regs.end();) {
    if (*it && (*it)->db_id == id) {
      if ((*it)->regc) {
        destroyRegcHandle((*it)->regc, id, sendUnregister);
        (*it)->regc = nullptr;
      }
      it = g_regs.erase(it);
    } else {
      ++it;
    }
  }
}

static void removeRouteById(int64_t id) {
  g_routes.erase(std::remove_if(g_routes.begin(), g_routes.end(),
                                [id](const CachedRoute& r) { return r.id == id; }),
                 g_routes.end());
}

static bool startRegForRow(int64_t id, const std::string& sip_ip, int sip_port, const std::string& transport,
                           const std::string& upstream_gb_id, const std::string& sip_domain,
                           const std::string& reg_username, const std::string& reg_password, int reg_expires_sec,
                           int hb_interval);

/** @details 与 reloadFromDb 单行解析一致：更新 g_routes，必要时 startRegForRow */
static void applyUpstreamPlatformRowParsed(const std::vector<std::string>& c) {
  if (c.size() < 11) return;
  int64_t id = std::atoll(trimSql(c[0]).c_str());
  std::string sip_ip = trimSql(c[1]);
  int sip_port = std::atoi(trimSql(c[2]).c_str());
  std::string transport = trimSql(c[3]);
  std::string gb_id = trimSql(c[4]);
  std::string sip_domain = trimSql(c[5]);
  std::string reg_u = trimSql(c[6]);
  std::string reg_p = trimSql(c[7]);
  bool enabled = (trimSql(c[8]) == "t" || trimSql(c[8]) == "true");
  int regExp = std::atoi(trimSql(c[9]).c_str());
  if (regExp <= 0) regExp = 3600;
  int hb = std::atoi(trimSql(c[10]).c_str());
  if (hb <= 0) hb = 60;
  (void)transport;
  (void)reg_u;

  g_routes.push_back(CachedRoute{id, gb_id, sip_ip, sip_port, enabled});

  if (!enabled) {
    updateUpstreamOnline(id, false);
    return;
  }
  if (sip_ip.empty() || sip_port <= 0 || gb_id.empty() || sip_domain.empty()) {
    updateUpstreamOnline(id, false);
    return;
  }

  if (!startRegForRow(id, sip_ip, sip_port, transport, gb_id, sip_domain, reg_u, reg_p, regExp, hb)) {
    updateUpstreamOnline(id, false);
  }
}

static bool startRegForRow(int64_t id, const std::string& sip_ip, int sip_port, const std::string& transport,
                           const std::string& upstream_gb_id, const std::string& sip_domain,
                           const std::string& reg_username, const std::string& reg_password, int reg_expires_sec,
                           int hb_interval) {
  (void)transport;
  if (!g_sip_endpt || sip_ip.empty() || sip_port <= 0) return false;

  std::string localGb = getSystemGbId();
  if (localGb.empty()) localGb = "34020000002000000001";
  std::string localHost = "127.0.0.1";
  int localPort = 5060;
  std::string out = execPsql(
      "SELECT COALESCE(NULLIF(TRIM(signal_ip),''), '127.0.0.1'), COALESCE(signal_port, 5060) FROM "
      "gb_local_config LIMIT 1");
  if (!out.empty()) {
    size_t p = out.find('|');
    if (p != std::string::npos) {
      localHost = trimSql(out.substr(0, p));
      std::string ps = trimSql(out.substr(p + 1));
      if (!ps.empty()) localPort = std::atoi(ps.c_str());
    }
  }
  if (localHost.empty()) localHost = "127.0.0.1";

  (void)reg_username;
  int expiresSec = reg_expires_sec > 0 ? reg_expires_sec : 3600;
  if (expiresSec < 60) expiresSec = 60;
  if (expiresSec > 86400) expiresSec = 86400;
  // Request-URI / To：上级国标 ID @ 上级信令 IP/端口（与 upstream_platforms.sip_ip、sip_port 一致）；
  // From / Contact：本级国标 ID @ 本机 signal_ip/signal_port；Digest 用户名与 From user 一致（本级国标 ID）。
  std::string upHost = sipHostPortLiteral(sip_ip, sip_port);
  std::string locHost = sipHostPortLiteral(localHost, localPort);
  std::string registrarUri = "sip:" + upstream_gb_id + "@" + upHost;
  std::string fromUri = "sip:" + localGb + "@" + locHost;
  std::string toUri = "sip:" + upstream_gb_id + "@" + upHost;
  std::string contactStr = "sip:" + localGb + "@" + locHost;
  const std::string routeUri = "sip:" + upstream_gb_id + "@" + upHost + ";lr";

  auto entry = std::make_unique<RegEntry>();
  entry->db_id = id;
  entry->sip_ip = sip_ip;
  entry->sip_port = sip_port;
  entry->upstream_gb_id = upstream_gb_id;
  entry->sip_domain = sip_domain;
  entry->reg_username = localGb;
  entry->reg_password = reg_password;
  entry->heartbeat_interval = hb_interval > 0 ? hb_interval : 60;
  entry->enabled = true;
  entry->registration_ok = false;
  entry->register_inflight = false;
  entry->keepalive_failures = 0;
  entry->register_retry_attempt = getRegisterRetryAttempt(id);
  // 避免首次 REGISTER 仍在事务中时，同轮维护又立即触发重建重注册。
  entry->next_register_retry = std::chrono::steady_clock::now() + std::chrono::seconds(5);

  pjsip_regc* regc = nullptr;
  pj_status_t st = pjsip_regc_create(g_sip_endpt, entry.get(), on_regc_cb, &regc);
  if (st != PJ_SUCCESS || !regc) {
    ErrorL << "【上级REGISTER】pjsip_regc_create 失败 id=" << id << " st=" << st;
    return false;
  }

  pj_str_t srv_pj = pj_str((char*)registrarUri.c_str());
  pj_str_t from_pj = pj_str((char*)fromUri.c_str());
  pj_str_t to_pj = pj_str((char*)toUri.c_str());
  pj_str_t contact_pj = pj_str((char*)contactStr.c_str());

  st = pjsip_regc_init(regc, &srv_pj, &from_pj, &to_pj, 1, &contact_pj, expiresSec);
  if (st != PJ_SUCCESS) {
    ErrorL << "【上级REGISTER】pjsip_regc_init 失败 id=" << id << " st=" << st;
    pjsip_regc_destroy(regc);
    return false;
  }

  {
    pj_pool_t* rtPool = pjsip_endpt_create_pool(g_sip_endpt, "upreg_rt", 512, 256);
    if (rtPool) {
      pjsip_route_hdr route_list;
      pj_list_init(&route_list);
      pjsip_route_hdr* rh = pjsip_route_hdr_create(rtPool);
      pj_str_t rustr = pj_str((char*)routeUri.c_str());
      pjsip_uri* ruri = pjsip_parse_uri(rtPool, rustr.ptr, rustr.slen, PJSIP_PARSE_URI_AS_NAMEADDR);
      if (ruri && rh) {
        rh->name_addr.uri = ruri;
        pj_list_push_back(&route_list, rh);
        pj_status_t rst = pjsip_regc_set_route_set(regc, &route_list);
        if (rst != PJ_SUCCESS) {
          WarnL << "【上级REGISTER】pjsip_regc_set_route_set 失败 id=" << id << " st=" << rst;
        }
      } else {
        WarnL << "【上级REGISTER】Route URI 解析失败 id=" << id << " uri=" << routeUri;
      }
      pjsip_endpt_release_pool(g_sip_endpt, rtPool);
    }
  }

  if (!reg_password.empty()) {
    pjsip_cred_info cred[1];
    pj_bzero(cred, sizeof(cred));
    cred[0].realm = pj_str((char*)"*");
    cred[0].scheme = pj_str((char*)"digest");
    cred[0].username = pj_str((char*)localGb.c_str());
    cred[0].data_type = PJSIP_CRED_DATA_PLAIN_PASSWD;
    cred[0].data = pj_str((char*)reg_password.c_str());
    pjsip_regc_set_credentials(regc, 1, cred);
  }

  pjsip_tx_data* tdata = nullptr;
  st = pjsip_regc_register(regc, PJ_TRUE, &tdata);
  if (st != PJ_SUCCESS || !tdata) {
    ErrorL << "【上级REGISTER】pjsip_regc_register 失败 id=" << id << " st=" << st;
    pjsip_regc_destroy(regc);
    return false;
  }

  st = pjsip_regc_send(regc, tdata);
  if (st != PJ_SUCCESS) {
    ErrorL << "【上级REGISTER】pjsip_regc_send 失败 id=" << id << " st=" << st;
    pjsip_regc_destroy(regc);
    return false;
  }

  markRegisterBurst(*entry);
  entry->register_inflight = true;
  entry->regc = regc;
  g_regs.push_back(std::move(entry));
  InfoL << "【上级REGISTER】已发送 id=" << id << " Request-URI=" << registrarUri << " From=" << fromUri
        << " To=" << toUri << " Route=" << routeUri;
  return true;
}

static void reloadFromDb() {
  destroyAllRegc();
  g_routes.clear();

  std::string q =
      "SELECT id, sip_ip, sip_port, transport, gb_id, sip_domain, COALESCE(reg_username,''), "
      "COALESCE(reg_password,''), enabled, COALESCE(register_expires,3600), COALESCE(heartbeat_interval,60) "
      "FROM upstream_platforms ORDER BY id ASC";
  std::string out = execPsql(q.c_str());
  std::vector<std::string> lines;
  splitLines(out, lines);

  for (const auto& ln : lines) {
    std::vector<std::string> c = splitPipe(ln);
    applyUpstreamPlatformRowParsed(c);
  }
}

static void upsertUpstreamPlatformFromDb(int64_t id) {
  if (id <= 0) return;
  // 离线重试重建时不发 Expires=0，避免无意义注销包放大抓包噪音。
  destroyRegcForId(id, false);
  removeRouteById(id);

  std::string q =
      "SELECT id, sip_ip, sip_port, transport, gb_id, sip_domain, COALESCE(reg_username,''), "
      "COALESCE(reg_password,''), enabled, COALESCE(register_expires,3600), COALESCE(heartbeat_interval,60) "
      "FROM upstream_platforms WHERE id=" +
      std::to_string(id) + " LIMIT 1";
  std::string out = execPsql(q.c_str());
  std::vector<std::string> lines;
  splitLines(out, lines);
  if (lines.empty()) {
    updateUpstreamOnline(id, false);
    return;
  }
  std::vector<std::string> c = splitPipe(lines[0]);
  if (c.size() < 11) {
    updateUpstreamOnline(id, false);
    return;
  }
  applyUpstreamPlatformRowParsed(c);
}

static void removeUpstreamPlatformById(int64_t id) {
  if (id <= 0) return;
  destroyRegcForId(id);
  removeRouteById(id);
  clearRegisterRetryAttempt(id);
}

static void sendKeepaliveForEntry(RegEntry& e) {
  if (!e.registration_ok) return;
  // 与 REGISTER From 用户部分一致（reg_username 在 startReg 中已含本地国标回落）
  std::string kaId = e.reg_username;
  if (kaId.empty()) {
    kaId = getSystemGbId();
    if (kaId.empty()) kaId = "34020000002000000001";
  }
  static int sn = 1;
  std::ostringstream xml;
  xml << "<?xml version=\"1.0\"?>\r\n<Notify>\r\n<CmdType>Keepalive</CmdType>\r\n<SN>" << (sn++)
      << "</SN>\r\n<DeviceID>" << xmlEscapeText(kaId) << "</DeviceID>\r\n<Status>OK</Status>\r\n</Notify>\r\n";
  bool ok = sendUpstreamManscdpMessage(e.sip_ip, e.sip_port, e.upstream_gb_id, e.sip_ip, e.sip_port, xml.str());
  if (ok) {
    e.keepalive_failures = 0;
    std::string sql = "UPDATE upstream_platforms SET last_heartbeat_at=CURRENT_TIMESTAMP WHERE id=" +
                      std::to_string(e.db_id);
    execPsqlCommand(sql);
    return;
  }
  e.keepalive_failures++;
  WarnL << "【上级保活】发送失败 id=" << e.db_id << " 连续失败=" << e.keepalive_failures;
  if (e.keepalive_failures >= kKeepaliveFailThreshold && e.registration_ok) {
    WarnL << "【上级保活】连续失败达到阈值，主动置离线并触发重注册 id=" << e.db_id;
    e.registration_ok = false;
    updateUpstreamOnline(e.db_id, false);
    e.register_retry_attempt = 0;
    setRegisterRetryAttempt(e.db_id, 0);
    e.next_register_retry = std::chrono::steady_clock::now();
  }
}

static void processRetryTasks() {
  std::vector<RetryTask> snapshot;
  {
    std::lock_guard<std::mutex> lk(g_retry_mu);
    snapshot.swap(g_retry_tasks);
  }
  if (snapshot.empty()) return;
  auto now = std::chrono::steady_clock::now();
  std::vector<RetryTask> keep;
  keep.reserve(snapshot.size());
  for (auto& t : snapshot) {
    if (t.next_attempt.time_since_epoch().count() != 0 && now < t.next_attempt) {
      keep.emplace_back(std::move(t));
      continue;
    }
    if (t.deadline.time_since_epoch().count() != 0 && now > t.deadline) {
      WarnL << "【上级发信重试】超时丢弃 type=" << (t.type == RetryTaskType::CatalogNotify ? "notify" : "query-rsp")
            << " upstreamId=" << t.upstreamDbId << " 已重试=" << t.attempt;
      continue;
    }
    bool ok = sendUpstreamManscdpMessage(t.sip_ip, t.sip_port, t.gb_id, t.sip_ip, t.sip_port, t.body, t.method);
    if (ok) {
      continue;
    }
    t.attempt++;
    if (t.attempt > t.max_retries) {
      ErrorL << "【上级发信重试】达到最大重试次数后丢弃 type="
             << (t.type == RetryTaskType::CatalogNotify ? "notify" : "query-rsp")
             << " upstreamId=" << t.upstreamDbId << " max=" << t.max_retries;
      continue;
    }
    int delay = computeRetryDelaySec(t, t.attempt);
    t.next_attempt = now + std::chrono::seconds(delay);
    keep.emplace_back(std::move(t));
  }
  if (!keep.empty()) {
    std::lock_guard<std::mutex> lk(g_retry_mu);
    for (auto& t : keep) g_retry_tasks.emplace_back(std::move(t));
  }
}

}  // namespace

void requestUpstreamRegistrarReload() {
  std::lock_guard<std::mutex> lk(g_inc_mu);
  g_inc_ops.clear();
  g_full_reload_requested.store(true);
}

void requestUpstreamRegistrarSyncRow(int64_t id) {
  if (id <= 0) return;
  std::lock_guard<std::mutex> lk(g_inc_mu);
  if (g_full_reload_requested.load()) return;
  g_inc_ops.emplace_back('U', id);
}

void requestUpstreamRegistrarRemoveRow(int64_t id) {
  if (id <= 0) return;
  std::lock_guard<std::mutex> lk(g_inc_mu);
  if (g_full_reload_requested.load()) return;
  g_inc_ops.emplace_back('D', id);
}

UpstreamSignalMatch matchUpstreamBySignalSource(const char* srcIp, int srcPort) {
  UpstreamSignalMatch m;
  if (!srcIp || srcIp[0] == '\0') return m;
  std::lock_guard<std::mutex> lock(g_mu);
  int64_t disabledCandidateId = 0;
  for (const auto& r : g_routes) {
    if (r.sip_ip == srcIp && r.sip_port == srcPort) {
      // 同一 signal 源可能存在多条上级配置（历史数据/重复配置）：
      // 优先命中 enabled=true 的记录，避免被先遍历到的 disabled 记录误拦截为 403。
      if (r.enabled) {
        m.matched = true;
        m.platformDbId = r.id;
        m.enabled = true;
        return m;
      }
      if (disabledCandidateId <= 0) {
        disabledCandidateId = r.id;
      }
    }
  }
  if (disabledCandidateId > 0) {
    m.matched = true;
    m.platformDbId = disabledCandidateId;
    m.enabled = false;
  }
  return m;
}

UpstreamSignalMatch matchUpstreamByGbId(const char* gbId) {
  UpstreamSignalMatch m;
  if (!gbId || gbId[0] == '\0') return m;
  std::lock_guard<std::mutex> lock(g_mu);
  int64_t disabledCandidateId = 0;
  for (const auto& r : g_routes) {
    if (r.gb_id == gbId) {
      if (r.enabled) {
        m.matched = true;
        m.platformDbId = r.id;
        m.enabled = true;
        return m;
      }
      if (disabledCandidateId <= 0) {
        disabledCandidateId = r.id;
      }
    }
  }
  if (disabledCandidateId > 0) {
    m.matched = true;
    m.platformDbId = disabledCandidateId;
    m.enabled = false;
  }
  return m;
}

void upstreamRegistrarProcessMaintenance() {
  bool full = g_full_reload_requested.exchange(false);
  std::vector<std::pair<char, int64_t>> batch;
  {
    std::lock_guard<std::mutex> lk(g_inc_mu);
    if (full) {
      g_inc_ops.clear();
    } else {
      batch.swap(g_inc_ops);
    }
  }

  if (full) {
    std::lock_guard<std::mutex> lock(g_mu);
    reloadFromDb();
  } else if (!batch.empty()) {
    std::lock_guard<std::mutex> lock(g_mu);
    for (const auto& op : batch) {
      if (op.first == 'D')
        removeUpstreamPlatformById(op.second);
      else
        upsertUpstreamPlatformFromDb(op.second);
    }
  }

  auto now = std::chrono::steady_clock::now();
  std::vector<RegEntry*> ka;
  std::vector<int64_t> registerRetryIds;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    for (auto& e : g_regs) {
      if (!e || !e->enabled) continue;
      if (!e->registration_ok) {
        if (!e->register_inflight &&
            (e->next_register_retry.time_since_epoch().count() == 0 || now >= e->next_register_retry)) {
          registerRetryIds.push_back(e->db_id);
          int delay = computeRegisterRetryDelaySec(e->register_retry_attempt);
          e->next_register_retry = now + std::chrono::seconds(delay);
        }
        continue;
      }
      int interval = e->heartbeat_interval > 0 ? e->heartbeat_interval : 60;
      if (e->next_keepalive.time_since_epoch().count() == 0 ||
          now >= e->next_keepalive + std::chrono::seconds(interval)) {
        ka.push_back(e.get());
        e->next_keepalive = now;
      }
    }
  }
  for (RegEntry* e : ka) {
    if (e) sendKeepaliveForEntry(*e);
  }
  if (!registerRetryIds.empty()) {
    std::lock_guard<std::mutex> lock(g_mu);
    for (int64_t id : registerRetryIds) {
      upsertUpstreamPlatformFromDb(id);
    }
  }

  std::vector<int64_t> notifyIds;
  {
    std::lock_guard<std::mutex> lk(g_notify_mu);
    notifyIds.swap(g_pending_catalog_notify);
  }
  static int catalogNotifySn = 1;
  for (int64_t uid : notifyIds) {
    std::vector<std::string> xmlParts;
    int snStart = catalogNotifySn;
    if (!buildUpstreamCatalogNotifyXmlParts(uid, catalogNotifySn, xmlParts)) {
      WarnL << "【上级目录上报】构建 XML 失败 upstreamId=" << uid;
      continue;
    }
    // 发信目标必须与上级保活/REGISTER 一致：以库表为准，避免仅依赖 g_routes 缓存未同步时静默不发 SIP
    std::string row = execPsql(
        ("SELECT COALESCE(TRIM(gb_id),''), COALESCE(NULLIF(TRIM(sip_ip),''), ''), "
         "COALESCE(sip_port,5060)::text FROM upstream_platforms WHERE id=" +
         std::to_string(uid) + " LIMIT 1")
            .c_str());
    std::string gb_id;
    std::string sip_ip;
    int sip_port = 5060;
    if (!row.empty()) {
      std::vector<std::string> cols = splitPipe(row);
      if (cols.size() >= 3) {
        gb_id = trimSql(cols[0]);
        sip_ip = trimSql(cols[1]);
        sip_port = std::max(1, std::atoi(trimSql(cols[2]).c_str()));
      }
    }
    if (row.empty() || gb_id.empty()) {
      WarnL << "【上级目录上报】未配置或缺少 gb_id，跳过发信 upstreamId=" << uid;
      continue;
    }
    if (sip_ip.empty()) {
      WarnL << "【上级目录上报】未配置 sip_ip，跳过发信 upstreamId=" << uid << "（请在平台配置中填写上级信令地址）";
      continue;
    }
    for (size_t pi = 0; pi < xmlParts.size(); ++pi) {
      RetryTask task;
      task.type = RetryTaskType::CatalogNotify;
      task.upstreamDbId = uid;
      task.sip_ip = sip_ip;
      task.sip_port = sip_port;
      task.gb_id = gb_id;
      task.method = "NOTIFY";
      task.body = xmlParts[pi];
      task.max_retries = 8;
      task.base_delay_sec = 2;
      task.max_delay_sec = 300;
      task.jitter_ratio = 0.2;
      task.next_attempt = std::chrono::steady_clock::now();
      task.deadline = task.next_attempt + std::chrono::minutes(30);
      enqueueRetryTask(std::move(task));
      InfoL << "【上级目录上报】入重试队列 upstreamId=" << uid << " SN段起始=" << snStart << " 第" << (pi + 1)
            << "/" << xmlParts.size() << " 包 字节≈" << xmlParts[pi].size() << " 目标=" << sip_ip << ":" << sip_port;
    }
  }

  // ---- 处理上级目录检索应答队列（用 MESSAGE 分包发送 Response XML） ----
  std::vector<PendingQueryResponse> queryRspList;
  {
    std::lock_guard<std::mutex> lk(g_query_rsp_mu);
    queryRspList.swap(g_pending_query_rsp);
  }
  for (const auto& qr : queryRspList) {
    std::vector<std::string> xmlParts;
    int sn = qr.sn;
    if (!buildUpstreamCatalogNotifyXmlParts(qr.upstreamDbId, sn, xmlParts)) {
      WarnL << "【上级目录检索应答】构建 XML 失败 upstreamId=" << qr.upstreamDbId;
      continue;
    }
    std::string row = execPsql(
        ("SELECT COALESCE(TRIM(gb_id),''), COALESCE(NULLIF(TRIM(sip_ip),''), ''), "
         "COALESCE(sip_port,5060)::text FROM upstream_platforms WHERE id=" +
         std::to_string(qr.upstreamDbId) + " LIMIT 1")
            .c_str());
    std::string gb_id, sip_ip;
    int sip_port = 5060;
    if (!row.empty()) {
      std::vector<std::string> cols = splitPipe(row);
      if (cols.size() >= 3) {
        gb_id = trimSql(cols[0]);
        sip_ip = trimSql(cols[1]);
        sip_port = std::max(1, std::atoi(trimSql(cols[2]).c_str()));
      }
    }
    if (gb_id.empty() || sip_ip.empty()) {
      WarnL << "【上级目录检索应答】缺少 gb_id 或 sip_ip，跳过 upstreamId=" << qr.upstreamDbId;
      continue;
    }
    // GB28181：目录检索应答使用 MESSAGE 方法（区别于主动推送的 NOTIFY）
    for (size_t pi = 0; pi < xmlParts.size(); ++pi) {
      // 关键修复：目录检索应答分包必须复用“查询 SN”，不能每包自增 SN。
      forceCatalogResponseSn(xmlParts[pi], qr.sn);
      RetryTask task;
      task.type = RetryTaskType::CatalogQueryResponse;
      task.upstreamDbId = qr.upstreamDbId;
      task.sip_ip = sip_ip;
      task.sip_port = sip_port;
      task.gb_id = gb_id;
      task.method = "MESSAGE";
      task.body = xmlParts[pi];
      task.max_retries = 6;
      task.base_delay_sec = 2;
      task.max_delay_sec = 30;
      task.jitter_ratio = 0.2;
      task.next_attempt = std::chrono::steady_clock::now();
      task.deadline = task.next_attempt + std::chrono::seconds(120);
      enqueueRetryTask(std::move(task));
      InfoL << "【上级目录检索应答】入重试队列 upstreamId=" << qr.upstreamDbId << " SN=" << qr.sn << " 第"
            << (pi + 1) << "/" << xmlParts.size() << " 包 字节≈" << xmlParts[pi].size() << " 目标=" << sip_ip
            << ":" << sip_port;
    }
  }
  processRetryTasks();
}

bool enqueueUpstreamCatalogNotify(int64_t upstreamDbId) {
  if (upstreamDbId <= 0) return false;
  std::lock_guard<std::mutex> lk(g_notify_mu);
  g_pending_catalog_notify.push_back(upstreamDbId);
  return true;
}

void enqueueUpstreamCatalogQueryResponse(int64_t upstreamDbId, int sn) {
  std::lock_guard<std::mutex> lk(g_query_rsp_mu);
  g_pending_query_rsp.push_back({upstreamDbId, sn});
}

}  // namespace gb
