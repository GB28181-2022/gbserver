/**
 * @file MediaService.cpp
 * @brief 媒体服务协调层实现
 * @details 实现ZLM HTTP API调用、流会话管理和Web Hook处理
 * @date 2025
 */

#include "infra/MediaService.h"
#include "infra/AuthHelper.h"
#include "infra/DbUtil.h"
#include "infra/SipCatalog.h"
#include "Util/logger.h"
#include <iostream>
#include <sstream>
#include <random>
#include <curl/curl.h>
#include <cstring>
#include <vector>

using namespace toolkit;

namespace gb {

bool upstreamBridgeTryTeardownForRtpServerTimeout(const std::string& streamId);

namespace {

std::string trimDbField(std::string value) {
    size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    value = value.substr(first);
    while (!value.empty() &&
           (value.back() == '\r' || value.back() == '\n' ||
            value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    return value;
}

/** ZLM application/x-www-form-urlencoded 参数值编码（与 isStreamExistsInZlm / startSendRtp 共用） */
std::string zlmFormEscapeComponent(const std::string& s) {
    CURL* c = curl_easy_init();
    if (!c) {
        return s;
    }
    char* e = curl_easy_escape(c, s.c_str(), static_cast<int>(s.size()));
    std::string out = e ? e : s;
    if (e) {
        curl_free(e);
    }
    curl_easy_cleanup(c);
    return out;
}

StreamSessionStatus parseSessionStatus(const std::string& status) {
    if (status == "inviting") return StreamSessionStatus::INVITING;
    if (status == "streaming") return StreamSessionStatus::STREAMING;
    if (status == "closing") return StreamSessionStatus::CLOSING;
    if (status == "closed") return StreamSessionStatus::CLOSED;
    return StreamSessionStatus::INIT;
}

bool ensureMediaConfigPreviewTimeoutColumn() {
    if (!execPsqlCommand(
            "ALTER TABLE media_config ADD COLUMN IF NOT EXISTS preview_invite_timeout_sec INTEGER "
            "DEFAULT 45")) {
        return false;
    }
    execPsqlCommand(
        "UPDATE media_config SET preview_invite_timeout_sec = 45 WHERE id = 1 AND "
        "preview_invite_timeout_sec IS NULL");
    return true;
}

bool ensureStreamSessionsTable() {
    const std::string sql =
        "CREATE TABLE IF NOT EXISTS stream_sessions ("
        "id SERIAL PRIMARY KEY,"
        "stream_id VARCHAR(128) NOT NULL UNIQUE,"
        "camera_id VARCHAR(64) NOT NULL,"
        "device_gb_id VARCHAR(64) NOT NULL,"
        "platform_gb_id VARCHAR(64) NOT NULL,"
        "zlm_port INTEGER NOT NULL DEFAULT 0,"
        "call_id VARCHAR(128),"
        "status VARCHAR(16) DEFAULT 'init',"
        "flv_url VARCHAR(256),"
        "viewer_count INTEGER DEFAULT 0,"
        "is_active BOOLEAN DEFAULT FALSE,"
        "updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_stream_sessions_stream_id ON stream_sessions(stream_id);"
        "CREATE INDEX IF NOT EXISTS idx_stream_sessions_call_id ON stream_sessions(call_id);"
        "ALTER TABLE stream_sessions ADD COLUMN IF NOT EXISTS camera_db_id BIGINT;"
        "CREATE INDEX IF NOT EXISTS idx_stream_sessions_camera_db_id ON stream_sessions(camera_db_id);";
    return execPsqlCommand(sql);
}

void persistSessionRecord(const std::shared_ptr<StreamSession>& session) {
    if (!session) {
        return;
    }

    const std::string dbIdVal =
        session->cameraDbId.empty() ? "NULL" : session->cameraDbId;

    const std::string sql =
        "INSERT INTO stream_sessions ("
        "stream_id, camera_id, device_gb_id, platform_gb_id, camera_db_id, zlm_port, call_id, status, flv_url, viewer_count, is_active, updated_at"
        ") VALUES ('" +
        gb::escapeSqlString(session->streamId) + "', '" +
        gb::escapeSqlString(session->cameraId) + "', '" +
        gb::escapeSqlString(session->deviceGbId) + "', '" +
        gb::escapeSqlString(session->platformGbId) + "', " + dbIdVal + ", " +
        std::to_string(session->zlmPort) + ", '" +
        gb::escapeSqlString(session->callId) + "', '" +
        gb::escapeSqlString(getStreamStatusName(session->status)) + "', '" +
        gb::escapeSqlString(session->flvUrl) + "', " +
        std::to_string(session->viewerCount) + ", " +
        (session->isActive ? "TRUE" : "FALSE") +
        ", CURRENT_TIMESTAMP) "
        "ON CONFLICT (stream_id) DO UPDATE SET "
        "camera_id = EXCLUDED.camera_id, "
        "device_gb_id = EXCLUDED.device_gb_id, "
        "platform_gb_id = EXCLUDED.platform_gb_id, "
        "camera_db_id = COALESCE(EXCLUDED.camera_db_id, stream_sessions.camera_db_id), "
        "zlm_port = EXCLUDED.zlm_port, "
        "call_id = COALESCE(NULLIF(EXCLUDED.call_id, ''), stream_sessions.call_id), "
        "status = EXCLUDED.status, "
        "flv_url = COALESCE(NULLIF(EXCLUDED.flv_url, ''), stream_sessions.flv_url), "
        "viewer_count = EXCLUDED.viewer_count, "
        "is_active = EXCLUDED.is_active, "
        "updated_at = CURRENT_TIMESTAMP;";
    execPsqlCommand(sql);
}

void markPersistedSessionClosed(const std::string& streamId) {
    if (streamId.empty()) {
        return;
    }
    const std::string sql =
        "UPDATE stream_sessions SET "
        "status='closed', is_active=FALSE, viewer_count=0, zlm_port=0, updated_at=CURRENT_TIMESTAMP "
        "WHERE stream_id='" + gb::escapeSqlString(streamId) + "';";
    execPsqlCommand(sql);
}

/**
 * @brief 从 media_config 读取 ZLM HTTP API 根地址（原始字符串，可能未规范化）
 */
std::string readDbZlmApiBaseUrlRaw() {
    if (!MediaService::ensureMediaApiUrlMigration()) {
        return "";
    }
    std::string out =
        execPsql("SELECT COALESCE(TRIM(media_api_url::text), '') FROM media_config WHERE id = 1");
    if (out.empty()) {
        return "";
    }
    size_t nl = out.find('\n');
    return trimDbField((nl == std::string::npos) ? out : out.substr(0, nl));
}

bool loadPersistedSession(const std::string& streamId, const std::shared_ptr<StreamSession>& session) {
    if (streamId.empty() || !session) {
        return false;
    }

    const std::string sql =
        "SELECT camera_id, device_gb_id, platform_gb_id, "
        "COALESCE(zlm_port, 0), COALESCE(call_id, ''), COALESCE(status, 'init'), "
        "COALESCE(flv_url, ''), COALESCE(viewer_count, 0), COALESCE(is_active::text, 'f'), "
        "COALESCE(camera_db_id::text, '') "
        "FROM stream_sessions WHERE stream_id='" + gb::escapeSqlString(streamId) + "' "
        "ORDER BY updated_at DESC LIMIT 1";
    std::string out = execPsql(sql.c_str());
    if (out.empty()) {
        return false;
    }

    size_t nl = out.find('\n');
    std::string line = trimDbField((nl == std::string::npos) ? out : out.substr(0, nl));
    if (line.empty()) {
        return false;
    }

    std::vector<std::string> cols;
    std::stringstream ss(line);
    std::string item;
    while (std::getline(ss, item, '|')) {
        cols.push_back(trimDbField(item));
    }
    if (cols.size() < 10) {
        return false;
    }

    session->cameraId = cols[0];
    session->deviceGbId = cols[1];
    session->platformGbId = cols[2];
    session->zlmPort = static_cast<uint16_t>(std::stoi(cols[3].empty() ? "0" : cols[3]));
    session->callId = cols[4];
    session->status = parseSessionStatus(cols[5]);
    session->flvUrl = cols[6];
    session->viewerCount = std::stoi(cols[7].empty() ? "0" : cols[7]);
    session->isActive = (cols[8] == "t" || cols[8] == "true" || cols[8] == "1");
    session->cameraDbId = cols[9];
    return true;
}

/** 库中 media_api_url 若被填成非法值（如仅「http://」），libcurl 报 URL malformed；回退为默认可访问的 ZLM API 根 */
static void sanitizeZlmApiBaseInPlace(std::string& base) {
  const char* kDefault = "http://127.0.0.1:880";
  auto trim = [](std::string& s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) {
      s.clear();
      return;
    }
    s = s.substr(a);
  };
  trim(base);
  if (base.empty()) {
    base = kDefault;
    return;
  }
  size_t scheme = base.find("://");
  if (scheme == std::string::npos) return;
  std::string rest = base.substr(scheme + 3);
  size_t slash = rest.find('/');
  std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
  trim(authority);
  if (authority.empty()) {
    std::cerr << "[MediaService] media_api_url 缺少主机:端口，已回退 " << kDefault << std::endl;
    base = kDefault;
  }
}

}  // namespace

// ========== 静态回调函数（用于libcurl） ==========

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// ========== 单例实现 ==========

MediaService& MediaService::instance() {
    static MediaService instance;
    return instance;
}

std::string MediaService::buildStreamId(const std::string& platformGbId,
                                        const std::string& cameraId) {
    if (platformGbId.empty()) {
        return cameraId;
    }
    return platformGbId + "_" + cameraId;
}

bool MediaService::ensureMediaApiUrlMigration() {
    static bool done = false;
    if (done) {
        return true;
    }
    std::string hasOldPort = execPsql(
        "SELECT 1 FROM information_schema.columns WHERE table_schema='public' "
        "AND table_name='media_config' AND column_name='media_http_port' LIMIT 1");
    if (!hasOldPort.empty()) {
        if (!execPsqlCommand("ALTER TABLE media_config ADD COLUMN IF NOT EXISTS media_api_url VARCHAR(512)")) {
            return false;
        }
        if (!execPsqlCommand(
                "UPDATE media_config SET media_api_url = "
                "'http://' || TRIM(COALESCE(NULLIF(media_http_host::text, ''), '127.0.0.1')) || ':' || "
                "CASE WHEN COALESCE(media_http_port, 0) <= 0 THEN '880' ELSE media_http_port::text END "
                "WHERE id = 1 AND (media_api_url IS NULL OR TRIM(COALESCE(media_api_url, '')) = '')")) {
            return false;
        }
        if (!execPsqlCommand("ALTER TABLE media_config DROP COLUMN IF EXISTS media_http_port")) {
            return false;
        }
    } else {
        execPsqlCommand("ALTER TABLE media_config ADD COLUMN IF NOT EXISTS media_api_url VARCHAR(512)");
    }
    done = true;
    return true;
}

std::string MediaService::normalizeMediaApiUrl(const std::string& input) {
    std::string s = input;
    size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    s = s.substr(first);
    while (!s.empty() &&
           (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
    }
    while (!s.empty() && s.back() == '/') {
        s.pop_back();
    }
    if (s.empty()) {
        return "";
    }
    if (s.find("://") == std::string::npos) {
        s = "http://" + s;
    }
    return s;
}

MediaService& GetMediaService() {
    return MediaService::instance();
}

// ========== 初始化与关闭 ==========

bool MediaService::initialize(const std::string& defaultApiBaseUrl, const std::string& zlmSecret) {
    if (initialized_) {
        return true;
    }

    std::string dbRaw = readDbZlmApiBaseUrlRaw();
    if (!dbRaw.empty()) {
        zlmApiBaseUrl_ = normalizeMediaApiUrl(dbRaw);
    } else {
        zlmApiBaseUrl_ = normalizeMediaApiUrl(defaultApiBaseUrl);
    }
    if (zlmApiBaseUrl_.empty()) {
        zlmApiBaseUrl_ = "http://127.0.0.1:880";
    }
    sanitizeZlmApiBaseInPlace(zlmApiBaseUrl_);
    zlmSecret_ = zlmSecret;

    // 从系统配置读取流媒体 IP（播放）与 Secret
    mediaServerIp_ = getMediaServerIp();
    if (mediaServerIp_.empty()) {
        mediaServerIp_ = "127.0.0.1";
        std::cerr << "[MediaService] Warning: mediaServerIp not configured, using 127.0.0.1" << std::endl;
    }

    // 如果没有传入 secret，从数据库读取
    if (zlmSecret_.empty()) {
        zlmSecret_ = getZlmSecret();
    }

    ensureStreamSessionsTable();
    reloadPreviewInviteTimeoutSec();
    reloadZlmOpenRtpServerWaitSec();

    initialized_ = true;
    std::cout << "[MediaService] Initialized with ZLM API base: " << zlmApiBaseUrl_
              << ", Media IP: " << mediaServerIp_
              << ", Secret: " << (zlmSecret_.empty() ? "(empty)" : "(configured)") << std::endl;
    return true;
}

void MediaService::refreshZlmRuntimeConfig() {
    std::string dbRaw = readDbZlmApiBaseUrlRaw();
    if (!dbRaw.empty()) {
        zlmApiBaseUrl_ = normalizeMediaApiUrl(dbRaw);
    } else {
        zlmApiBaseUrl_ = "http://127.0.0.1:880";
    }
    sanitizeZlmApiBaseInPlace(zlmApiBaseUrl_);
    zlmSecret_ = getZlmSecret();
    mediaServerIp_ = getMediaServerIp();
    reloadPreviewInviteTimeoutSec();
    reloadZlmOpenRtpServerWaitSec();
    std::cout << "[MediaService] Refreshed ZLM API base: " << zlmApiBaseUrl_ << std::endl;
}

void MediaService::reloadPreviewInviteTimeoutSec() {
    ensureMediaConfigPreviewTimeoutColumn();
    std::string out = execPsql(
        "SELECT COALESCE(preview_invite_timeout_sec, 45) FROM media_config WHERE id = 1 LIMIT 1");
    int v = 45;
    if (!out.empty()) {
        size_t nl = out.find('\n');
        std::string line = trimDbField((nl == std::string::npos) ? out : out.substr(0, nl));
        if (!line.empty()) {
            int parsed = std::atoi(line.c_str());
            if (parsed > 0) {
                v = parsed;
            }
        }
    }
    if (v < 10) {
        v = 10;
    }
    if (v > 600) {
        v = 600;
    }
    previewInviteTimeoutSec_.store(v, std::memory_order_relaxed);
}

int MediaService::getPreviewInviteTimeoutSec() const {
    return previewInviteTimeoutSec_.load(std::memory_order_relaxed);
}

void MediaService::reloadZlmOpenRtpServerWaitSec() {
    if (!execPsqlCommand(
            "ALTER TABLE media_config ADD COLUMN IF NOT EXISTS zlm_open_rtp_server_wait_sec INTEGER "
            "DEFAULT 10")) {
        return;
    }
    std::string out = execPsql(
        "SELECT COALESCE(zlm_open_rtp_server_wait_sec, 10) FROM media_config WHERE id = 1 LIMIT 1");
    int v = 10;
    if (!out.empty()) {
        size_t nl = out.find('\n');
        std::string line = trimDbField((nl == std::string::npos) ? out : out.substr(0, nl));
        if (!line.empty()) {
            int parsed = std::atoi(line.c_str());
            if (parsed > 0) {
                v = parsed;
            }
        }
    }
    if (v < 1) {
        v = 1;
    }
    if (v > 120) {
        v = 120;
    }
    zlmOpenRtpServerWaitSec_.store(v, std::memory_order_relaxed);
}

int MediaService::getZlmOpenRtpServerWaitSec() const {
    return zlmOpenRtpServerWaitSec_.load(std::memory_order_relaxed);
}

size_t MediaService::tickInvitingTimeouts() {
    const int timeoutSec = getPreviewInviteTimeoutSec();
    if (timeoutSec <= 0) {
        return 0;
    }

    struct Victim {
        std::string sessionId;
        std::string callId;
        std::string deviceGbId;
        std::string cameraId;
        std::string streamId;
        uint16_t zlmPort = 0;
    };
    std::vector<Victim> victims;
    const auto now = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        for (const auto& p : sessions_) {
            const auto& s = p.second;
            if (!s) {
                continue;
            }
            if (s->status != StreamSessionStatus::INVITING && s->status != StreamSessionStatus::INIT) {
                continue;
            }
            const auto age = std::chrono::duration_cast<std::chrono::seconds>(now - s->createTime).count();
            if (age >= timeoutSec) {
                victims.push_back({p.first, s->callId, s->deviceGbId, s->cameraId, s->streamId, s->zlmPort});
            }
        }
    }

    const std::string mediaIp = getMediaServerIp();
    for (const auto& v : victims) {
        WarnL << "[MediaService] preview_no_rtp_after_ack: INVITING/INIT timeout " << timeoutSec
              << "s, session=" << v.sessionId << " streamId=" << v.streamId << " callId=" << v.callId
              << " INVITE SDP c= 应对齐 " << mediaIp << " m=video 端口应对齐 ZLM openRtpServer="
              << v.zlmPort << "（请与下级抓包核对 RTP 目的地址）";

        if (!v.callId.empty()) {
            sendPlayByeAsync(v.callId, v.deviceGbId, v.cameraId);
        }

        bool shouldClose = false;
        {
            std::lock_guard<std::mutex> lock(sessionsMutex_);
            auto it = sessions_.find(v.sessionId);
            if (it != sessions_.end() && it->second &&
                (it->second->status == StreamSessionStatus::INVITING ||
                 it->second->status == StreamSessionStatus::INIT)) {
                it->second->errorMessage = "preview_no_rtp_after_ack";
                shouldClose = true;
            }
        }
        if (shouldClose) {
            closeSession(v.sessionId);
        }
    }

    return victims.size();
}

bool MediaService::pushRtpProxyPortRangeToZlm(const std::string& zlmApiBaseUrl,
                                              const std::string& secret,
                                              int startPort,
                                              int endPort,
                                              std::string& errMsg) {
    errMsg.clear();
    if (startPort <= 0 || endPort <= 0 || startPort > endPort) {
        errMsg = "无效的 RTP 端口区间";
        return false;
    }
    const std::string range = std::to_string(startPort) + "-" + std::to_string(endPort);

    CURL* curl = curl_easy_init();
    if (!curl) {
        errMsg = "网络初始化失败";
        return false;
    }

    char* escSecret = curl_easy_escape(curl, secret.c_str(), static_cast<int>(secret.size()));
    char* escRange = curl_easy_escape(curl, range.c_str(), static_cast<int>(range.size()));
    if (!escSecret || !escRange) {
        errMsg = "参数编码失败";
        if (escSecret) {
            curl_free(escSecret);
        }
        if (escRange) {
            curl_free(escRange);
        }
        curl_easy_cleanup(curl);
        return false;
    }

    std::string base = normalizeMediaApiUrl(zlmApiBaseUrl);
    if (base.empty()) {
        errMsg = "无效的 ZLM API 根地址";
        curl_free(escSecret);
        curl_free(escRange);
        curl_easy_cleanup(curl);
        return false;
    }
    std::string url = base + "/index/api/setServerConfig?secret=" + escSecret +
                      "&rtp_proxy.port_range=" + escRange;
    curl_free(escSecret);
    curl_free(escRange);

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    }
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        errMsg = std::string("连接 ZLM 失败: ") + curl_easy_strerror(res);
        return false;
    }
    if (httpCode != 200) {
        errMsg = "ZLM HTTP 状态码 " + std::to_string(httpCode);
        return false;
    }

    size_t codePos = response.find("\"code\"");
    if (codePos == std::string::npos) {
        errMsg = "ZLM 响应异常";
        return false;
    }
    int code = -1;
    sscanf(response.c_str() + codePos, "\"code\"%*[^0-9-]%d", &code);
    if (code != 0) {
        errMsg = "ZLM setServerConfig 失败 (code=" + std::to_string(code) + ")";
        size_t msgPos = response.find("\"msg\"");
        if (msgPos == std::string::npos) {
            msgPos = response.find("\"message\"");
        }
        if (msgPos != std::string::npos) {
            size_t colon = response.find(':', msgPos);
            if (colon != std::string::npos) {
                size_t q1 = response.find('"', colon);
                if (q1 != std::string::npos) {
                    size_t q2 = response.find('"', q1 + 1);
                    if (q2 != std::string::npos) {
                        errMsg += ": ";
                        errMsg += response.substr(q1 + 1, q2 - q1 - 1);
                    }
                }
            }
        }
        return false;
    }

    std::cout << "[MediaService] pushRtpProxyPortRangeToZlm ok, range=" << range
              << " @ " << base << std::endl;
    return true;
}

void MediaService::shutdown() {
    if (!initialized_) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    
    // 关闭所有活动会话
    std::vector<std::string> sessionIds;
    for (const auto& pair : sessions_) {
        sessionIds.push_back(pair.first);
    }
    
    for (const auto& sessionId : sessionIds) {
        closeSessionInternal(sessionId);
    }
    
    sessions_.clear();
    streamIdIndex_.clear();
    
    initialized_ = false;
    std::cout << "[MediaService] Shutdown complete" << std::endl;
}

// ========== ZLM HTTP API调用 ==========

int MediaService::callZlmApi(const std::string& api, 
                              const std::string& params, 
                              std::string& outResponse) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "[MediaService] Failed to init CURL" << std::endl;
        return -1;
    }
    
    std::string base = zlmApiBaseUrl_;
    sanitizeZlmApiBaseInPlace(base);
    if (base != zlmApiBaseUrl_) {
        zlmApiBaseUrl_ = base;
    }
    std::string url = zlmApiBaseUrl_ + api;
    // ZLM REST 要求 URL 上带 secret 参数；未传会返回 -300 Required parameter missed: "secret"
    url += (api.find('?') != std::string::npos ? "&" : "?");
    url += "secret=";
    {
        char* esc = curl_easy_escape(curl, zlmSecret_.c_str(), (int)zlmSecret_.size());
        if (esc) {
            url += esc;
            curl_free(esc);
        }
    }

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
    
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, params.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &outResponse);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    
    CURLcode res = curl_easy_perform(curl);
    
    long httpCode = 0;
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    } else {
        std::cerr << "[MediaService] CURL error: " << curl_easy_strerror(res) << std::endl;
        httpCode = -1;
    }
    
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    
    return static_cast<int>(httpCode);
}

bool MediaService::openRtpServer(const std::string& streamId, uint16_t& outPort, int tcpMode) {
    if (tcpMode < 0) tcpMode = 0;
    if (tcpMode > 2) tcpMode = 2;
    // 构造请求参数 - ZLM使用form-urlencoded格式（tcp_mode：0=UDP/1=TCP被动/2=TCP主动）
    std::string params = "stream_id=" + streamId + "&port=0&tcp_mode=" + std::to_string(tcpMode) +
                         "&re_use_port=0&ssrc=0";
    
    std::string response;
    int httpCode = callZlmApi("/index/api/openRtpServer", params, response);
    
    if (httpCode != 200) {
        std::cerr << "[MediaService] openRtpServer failed, HTTP code: " << httpCode << std::endl;
        return false;
    }
    
    // 解析响应JSON
    // ZLM响应格式: {"code" : 0, "port" : 30008} (注意ZLM使用制表符缩进和空格)
    size_t codePos = response.find("\"code\"");
    if (codePos == std::string::npos) {
        std::cerr << "[MediaService] Invalid ZLM response: " << response << std::endl;
        return false;
    }
    
    int code = 0;
    // 处理可能的空格和制表符: "code" : 0 或 "code":0
    sscanf(response.c_str() + codePos, "\"code\"%*[^0-9-]%d", &code);
    
    if (code != 0) {
        std::cerr << "[MediaService] openRtpServer ZLM error code: " << code << " body=" << response << std::endl;
        return false;
    }
    
    // 解析port - 处理可能的空格和制表符
    size_t portPos = response.find("\"port\"");
    if (portPos != std::string::npos) {
        sscanf(response.c_str() + portPos, "\"port\"%*[^0-9]%hu", &outPort);
    }
    
    if (outPort == 0) {
        std::cerr << "[MediaService] openRtpServer returned invalid port" << std::endl;
        return false;
    }
    
    std::cout << "[MediaService] openRtpServer success, streamId=" << streamId 
              << ", port=" << outPort << std::endl;
    return true;
}

bool MediaService::closeRtpServer(const std::string& streamId) {
    // ZLM使用form-urlencoded格式
    std::string params = "stream_id=" + streamId;

    std::string response;
    int httpCode = callZlmApi("/index/api/closeRtpServer", params, response);

    if (httpCode != 200) {
        std::cerr << "[MediaService] closeRtpServer failed, HTTP code: " << httpCode << std::endl;
        return false;
    }

    // 解析响应 - 处理可能的空格和制表符
    size_t codePos = response.find("\"code\"");
    if (codePos != std::string::npos) {
        int code = 0;
        sscanf(response.c_str() + codePos, "\"code\"%*[^0-9-]%d", &code);
        if (code != 0) {
            std::cerr << "[MediaService] closeRtpServer ZLM error code: " << code << std::endl;
            return false;
        }
    }

    std::cout << "[MediaService] closeRtpServer success, streamId=" << streamId << std::endl;
    return true;
}

bool MediaService::startSendRtpPs(const std::string& streamId,
                                  const std::string& ssrc,
                                  const std::string& dstIp,
                                  uint16_t dstPort,
                                  bool isUdp,
                                  uint16_t& outLocalPort) {
    outLocalPort = 0;
    if (streamId.empty() || ssrc.empty() || dstIp.empty() || dstPort == 0) {
        InfoL << "【startSendRtpPs】missing arg streamId=" << streamId << " ssrc_empty=" << (ssrc.empty() ? 1 : 0);
        return false;
    }
    /* startSendRtp 内部 MediaSource::find(vhost,app,stream,from_mp4)，与 getMediaList 列表不是同一路径；
     * GB 收流在 ZLM 里常登记为 fmp4 派生，缺省 from_mp4=0 可能找不到源，故对 from_mp4=0/1 各试一次 */
    const std::string escStream = zlmFormEscapeComponent(streamId);
    const std::string escVhost = zlmFormEscapeComponent("__defaultVhost__");
    const std::string escApp = zlmFormEscapeComponent("rtp");
    const std::string escSsrc = zlmFormEscapeComponent(ssrc);
    const std::string escDst = zlmFormEscapeComponent(dstIp);

    for (int attempt = 0; attempt < 2; ++attempt) {
        const char* fromMp4 = (attempt == 0) ? "0" : "1";
        std::string params = std::string("vhost=") + escVhost + "&app=" + escApp + "&stream=" + escStream +
                             "&ssrc=" + escSsrc + "&dst_url=" + escDst +
                             "&dst_port=" + std::to_string(static_cast<int>(dstPort)) +
                             "&is_udp=" + std::string(isUdp ? "1" : "0") + "&from_mp4=" + fromMp4;

        std::string response;
        const int httpCode = callZlmApi("/index/api/startSendRtp", params, response);
        if (httpCode != 200) {
            InfoL << "【startSendRtpPs】HTTP " << httpCode << " from_mp4=" << fromMp4
                  << " stream=" << streamId << " body=" << response.substr(0, 800);
            if (attempt == 1) {
                return false;
            }
            continue;
        }
        int code = -1;
        const size_t codePos = response.find("\"code\"");
        if (codePos != std::string::npos) {
            std::sscanf(response.c_str() + codePos, "\"code\"%*[^0-9-]%d", &code);
        }
        uint16_t lp = 0;
        const size_t lpp = response.find("\"local_port\"");
        if (lpp != std::string::npos) {
            std::sscanf(response.c_str() + lpp, "\"local_port\"%*[^0-9]%hu", &lp);
        }
        if (code == 0 && lp != 0) {
            outLocalPort = lp;
            if (attempt == 1) {
                WarnL << "【startSendRtpPs】from_mp4=1 成功，from_mp4=0 未找到 MediaSource（与 getMediaList 中 schema 多为 fmp4 等有关） stream="
                      << streamId;
            }
            InfoL << "【startSendRtpPs】ok stream=" << streamId << " -> " << dstIp << ":" << dstPort
                  << " local_port=" << outLocalPort << " from_mp4=" << fromMp4;
            return true;
        }
        InfoL << "【startSendRtpPs】ZLM code=" << code << " from_mp4=" << fromMp4
              << " stream=" << streamId << " body=" << response.substr(0, 800);
        if (attempt == 1) {
            return false;
        }
    }
    return false;
}

bool MediaService::stopSendRtpForStream(const std::string& streamId, const std::string& ssrc) {
    if (streamId.empty()) return false;
    std::string params = "vhost=__defaultVhost__&app=rtp&stream=" + streamId;
    if (!ssrc.empty()) {
        params += "&ssrc=" + ssrc;
    }
    std::string response;
    int httpCode = callZlmApi("/index/api/stopSendRtp", params, response);
    if (httpCode != 200) {
        InfoL << "【stopSendRtpForStream】HTTP " << httpCode << " stream=" << streamId;
        return false;
    }
    int code = -1;
    size_t codePos = response.find("\"code\"");
    if (codePos != std::string::npos) {
        sscanf(response.c_str() + codePos, "\"code\"%*[^0-9-]%d", &code);
    }
    return code == 0;
}

bool MediaService::startMp4Record(const std::string& streamId,
                                   const std::string& customizedPath,
                                   int maxSecond) {
    std::string params =
        "vhost=__defaultVhost__&app=rtp&stream=" + streamId + "&type=1";
    if (!customizedPath.empty()) {
        params += "&customized_path=" + customizedPath;
    }
    if (maxSecond > 0) {
        params += "&max_second=" + std::to_string(maxSecond);
    }
    std::string response;
    int httpCode = callZlmApi("/index/api/startRecord", params, response);
    if (httpCode != 200) {
        InfoL << "【startMp4Record】ZLM 返回 HTTP " << httpCode << " stream=" << streamId;
        return false;
    }
    size_t codePos = response.find("\"code\"");
    if (codePos == std::string::npos) return false;
    int code = -1;
    sscanf(response.c_str() + codePos, "\"code\"%*[^0-9-]%d", &code);
    InfoL << "【startMp4Record】stream=" << streamId
          << " customized_path=" << (customizedPath.empty() ? "(default)" : customizedPath)
          << " max_second=" << maxSecond << " code=" << code;
    return code == 0;
}

bool MediaService::stopMp4Record(const std::string& streamId) {
    std::string params = "vhost=__defaultVhost__&app=rtp&stream=" + streamId;
    std::string response;
    int httpCode = callZlmApi("/index/api/stopRecord", params, response);
    if (httpCode != 200) {
        return false;
    }
    size_t codePos = response.find("\"code\"");
    if (codePos == std::string::npos) return false;
    int code = -1;
    sscanf(response.c_str() + codePos, "\"code\"%*[^0-9-]%d", &code);
    return code == 0;
}

bool MediaService::getMp4RecordFile(const std::string& streamId,
                                    const std::string& period,
                                    std::string& outRootPath,
                                    std::vector<std::string>& outFiles) {
    std::string params = "vhost=__defaultVhost__&app=rtp&stream=" + streamId +
                         "&period=" + period;
    std::string response;
    int httpCode = callZlmApi("/index/api/getMp4RecordFile", params, response);
    if (httpCode != 200) return false;

    int code = -1;
    size_t codePos = response.find("\"code\"");
    if (codePos != std::string::npos) {
        sscanf(response.c_str() + codePos, "\"code\"%*[^0-9-]%d", &code);
    }
    if (code != 0) return false;

    // 解析 rootPath
    size_t rpPos = response.find("\"rootPath\"");
    if (rpPos != std::string::npos) {
        size_t q1 = response.find('"', rpPos + 10);
        if (q1 != std::string::npos) {
            size_t q2 = response.find('"', q1 + 1);
            if (q2 != std::string::npos) {
                outRootPath = response.substr(q1 + 1, q2 - q1 - 1);
            }
        }
    }

    // 解析 paths 数组中的文件名
    size_t pathsPos = response.find("\"paths\"");
    if (pathsPos != std::string::npos) {
        size_t arrStart = response.find('[', pathsPos);
        size_t arrEnd = response.find(']', arrStart);
        if (arrStart != std::string::npos && arrEnd != std::string::npos) {
            std::string arrBody = response.substr(arrStart + 1, arrEnd - arrStart - 1);
            size_t pos = 0;
            while (pos < arrBody.size()) {
                size_t q1 = arrBody.find('"', pos);
                if (q1 == std::string::npos) break;
                size_t q2 = arrBody.find('"', q1 + 1);
                if (q2 == std::string::npos) break;
                std::string item = arrBody.substr(q1 + 1, q2 - q1 - 1);
                if (!item.empty()) outFiles.push_back(item);
                pos = q2 + 1;
            }
        }
    }

    return true;
}

bool MediaService::deleteRecordDirectory(const std::string& streamId,
                                          const std::string& period) {
    std::string params = "vhost=__defaultVhost__&app=rtp&stream=" + streamId +
                         "&period=" + period;
    std::string response;
    int httpCode = callZlmApi("/index/api/deleteRecordDirectory", params, response);
    if (httpCode != 200) return false;

    int code = -1;
    size_t codePos = response.find("\"code\"");
    if (codePos != std::string::npos) {
        sscanf(response.c_str() + codePos, "\"code\"%*[^0-9-]%d", &code);
    }
    InfoL << "【deleteRecordDirectory】stream=" << streamId
          << " period=" << period << " code=" << code;
    return code == 0;
}

namespace {

/**
 * @brief 解析 getMediaList 顶层：code 与 data 是否为非空数组（避免误把 tracks 等内层 [] 当成 data）
 */
bool parseZlmGetMediaListTop(const std::string& resp, int& outCode, bool& outDataHasItems, std::string& outNote) {
    outCode = -999;
    outDataHasItems = false;
    outNote.clear();
    const size_t codePos = resp.find("\"code\"");
    if (codePos == std::string::npos) {
        outNote = "no_code_field";
        return false;
    }
    if (std::sscanf(resp.c_str() + codePos, "\"code\"%*[^0-9-]%d", &outCode) < 1) {
        outNote = "code_scan_fail";
        return false;
    }
    const size_t dataKey = resp.find("\"data\"");
    if (dataKey == std::string::npos) {
        /* ZLM 部分应答/瞬态仅含 {"code":0} 无 data 字段，按「无列表」处理，勿判解析失败 */
        outNote = "no_data_field";
        return true;
    }
    const size_t colon = resp.find(':', dataKey + 6);
    if (colon == std::string::npos) {
        outNote = "no_data_colon";
        return false;
    }
    size_t i = resp.find_first_not_of(" \t\r\n", colon + 1);
    if (i == std::string::npos) {
        outNote = "data_value_missing";
        return false;
    }
    if (resp.compare(i, 4, "null") == 0) {
        outNote = "data_null";
        return true;
    }
    if (resp[i] != '[') {
        outNote = "data_not_array";
        return true;
    }
    const size_t afterBracket = resp.find_first_not_of(" \t\r\n", i + 1);
    if (afterBracket == std::string::npos) {
        outNote = "data_arr_truncated";
        return true;
    }
    if (resp[afterBracket] == ']') {
        outNote = "data_empty_array";
        return true;
    }
    outDataHasItems = true;
    outNote = "data_has_items";
    return true;
}

bool jsonBodyContainsStreamField(const std::string& json, const std::string& streamId) {
    if (json.find(std::string("\"stream\":\"") + streamId + "\"") != std::string::npos) return true;
    if (json.find(std::string("\"stream\": \"") + streamId + "\"") != std::string::npos) return true;
    if (json.find(std::string("\"stream\" : \"") + streamId + "\"") != std::string::npos) return true;
    if (json.find(std::string("\"stream\" :\"") + streamId + "\"") != std::string::npos) return true;
    return false;
}

void logZlmResponseSnippet(const char* phase, const std::string& response) {
    constexpr size_t kMax = 2400;
    const std::string snippet =
        response.size() > kMax ? response.substr(0, kMax) + "...(truncated)" : response;
    InfoL << phase << " resp_len=" << response.size() << " body=" << snippet;
}

}  // namespace

bool MediaService::isStreamExistsInZlm(const std::string& streamId) {
    // ZLM /index/api/getMediaList：见 WebApi.cpp MediaSource::for_each_media(schema,vhost,app,stream)
    // 不按 schema 过滤（多 schema 并存）；stream/vhost/app 需 form 编码，否则特殊字符会导致过滤失效。
    if (streamId.empty()) {
        InfoL << "【getMediaList】streamId empty → false";
        return false;
    }

    const std::string escStream = zlmFormEscapeComponent(streamId);
    const std::string escVhost = zlmFormEscapeComponent("__defaultVhost__");
    const std::string escApp = zlmFormEscapeComponent("rtp");
    const std::string params = "stream=" + escStream + "&vhost=" + escVhost + "&app=" + escApp;

    std::string response;
    const int httpCode = callZlmApi("/index/api/getMediaList", params, response);

    int code = -999;
    bool dataHasItems = false;
    std::string parseNote;
    const bool parsedOk = parseZlmGetMediaListTop(response, code, dataHasItems, parseNote);

    const bool strictExists =
        (httpCode == 200) && parsedOk && (code == 0) && dataHasItems && (parseNote == "data_has_items");

    InfoL << "【getMediaList】strict streamId=" << streamId << " params=\"" << params << "\" http=" << httpCode
          << " parsed=" << (parsedOk ? 1 : 0) << " jsonCode=" << code << " dataHasItems=" << (dataHasItems ? 1 : 0)
          << " parseNote=" << parseNote << " exists=" << (strictExists ? 1 : 0);

    if (httpCode != 200 || !parsedOk) {
        logZlmResponseSnippet("【getMediaList】strict_http_or_parse_fail", response);
    } else if (code != 0 || !strictExists) {
        logZlmResponseSnippet("【getMediaList】strict_no_match_detail", response);
    }

    if (strictExists) {
        std::cout << "[MediaService] Stream " << streamId << " exists in ZLM via getMediaList" << std::endl;
        return true;
    }

    // 二次：仅 vhost+app，在整段 JSON 中查找 "stream":"<id>"（严格带 stream= 时 ZLM 未命中但流已登记时常能对照）
    const std::string broadParams = "vhost=" + escVhost + "&app=" + escApp;
    std::string broadResp;
    const int httpBroad = callZlmApi("/index/api/getMediaList", broadParams, broadResp);
    int codeBroad = -999;
    bool itemsBroad = false;
    std::string noteBroad;
    const bool parsedBroad = parseZlmGetMediaListTop(broadResp, codeBroad, itemsBroad, noteBroad);
    const bool grepMatch = jsonBodyContainsStreamField(broadResp, streamId);

    InfoL << "【getMediaList】broad_fallback vhost+app=rtp http=" << httpBroad << " parsed=" << (parsedBroad ? 1 : 0)
          << " jsonCode=" << codeBroad << " dataHasItems=" << (itemsBroad ? 1 : 0) << " parseNote=" << noteBroad
          << " grepStreamField=" << (grepMatch ? 1 : 0);

    if (httpBroad == 200 && parsedBroad && codeBroad == 0 && grepMatch) {
        WarnL << "【getMediaList】strict 未命中但 broad 列表含 stream=\"" << streamId
              << "\" → 按存在处理（请核对严格查询与 ZLM 登记 stream 是否完全一致）";
        return true;
    }

    if (httpBroad == 200 && parsedBroad && codeBroad == 0 && itemsBroad && !grepMatch) {
        logZlmResponseSnippet("【getMediaList】broad_has_streams_but_no_streamId_grep", broadResp);
    }

    return false;
}

// ========== 播放URL生成 ==========

std::string MediaService::getMediaServerIp() {
    // 从数据库media_config表读取流媒体IP
    std::string sql = "SELECT media_http_host FROM media_config WHERE id = 1";
    std::string out = execPsql(sql.c_str());
    
    if (!out.empty()) {
        // 去除换行和空白
        size_t end = out.find('\n');
        if (end != std::string::npos) {
            out = out.substr(0, end);
        }
        // 去除首尾空格
        size_t start = out.find_first_not_of(" \t\r\n");
        if (start != std::string::npos) {
            out = out.substr(start);
        }
        size_t last = out.find_last_not_of(" \t\r\n");
        if (last != std::string::npos) {
            out = out.substr(0, last + 1);
        }
    }
    
    return out.empty() ? "127.0.0.1" : out;
}

std::string MediaService::getZlmSecret() {
    // 从数据库media_config表读取ZLM API密钥
    std::string sql = "SELECT COALESCE(zlm_secret, '') FROM media_config WHERE id = 1";
    std::string out = execPsql(sql.c_str());

    if (!out.empty()) {
        // 去除换行和空白
        size_t end = out.find('\n');
        if (end != std::string::npos) {
            out = out.substr(0, end);
        }
        // 去除首尾空格
        size_t start = out.find_first_not_of(" \t\r\n");
        if (start != std::string::npos) {
            out = out.substr(start);
        }
        size_t last = out.find_last_not_of(" \t\r\n");
        if (last != std::string::npos) {
            out = out.substr(0, last + 1);
        }
    }

    return out;
}

std::string MediaService::generateFlvUrl(const std::string& streamId) {
    // URL格式: http://{media_ip}/zlm/rtp/{stream_id}.live.flv
    // 通过Nginx代理 /zlm/ 转发到ZLM 880端口
    // 每次都从数据库读取最新的媒体IP配置
    std::string mediaIp = getMediaServerIp();
    return "http://" + mediaIp + "/zlm/rtp/" + streamId + ".live.flv";
}

std::string MediaService::generateWsFlvUrl(const std::string& streamId) {
    // WebSocket-FLV格式: ws://{media_ip}/zlm/rtp/{stream_id}.live.flv
    // 每次都从数据库读取最新的媒体IP配置
    std::string mediaIp = getMediaServerIp();
    return "ws://" + mediaIp + "/zlm/rtp/" + streamId + ".live.flv";
}

// ========== 流会话管理 ==========

std::string MediaService::generateSessionId() {
    // 生成UUID格式会话ID
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 15);
    static const char* hex = "0123456789abcdef";
    
    std::string uuid;
    uuid.reserve(36);
    
    for (int i = 0; i < 8; i++) uuid += hex[dis(gen)];
    uuid += '-';
    for (int i = 0; i < 4; i++) uuid += hex[dis(gen)];
    uuid += '-';
    for (int i = 0; i < 4; i++) uuid += hex[dis(gen)];
    uuid += '-';
    for (int i = 0; i < 4; i++) uuid += hex[dis(gen)];
    uuid += '-';
    for (int i = 0; i < 12; i++) uuid += hex[dis(gen)];
    
    return uuid;
}

std::shared_ptr<StreamSession> MediaService::createSession(const std::string& cameraId,
                                                           const std::string& deviceGbId,
                                                           const std::string& platformGbId,
                                                           const std::string& cameraDbId) {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    const std::string streamId = buildStreamId(platformGbId, cameraId);
    
    // 检查是否已存在该流的会话
    auto it = streamIdIndex_.find(streamId);
    if (it != streamIdIndex_.end() && it->second->status != StreamSessionStatus::CLOSED) {
        std::cout << "[MediaService] Session already exists for stream " << streamId
                  << ", returning existing session" << std::endl;
        return it->second;
    }
    
    auto session = std::make_shared<StreamSession>();
    session->sessionId = generateSessionId();
    session->streamId = streamId;
    session->cameraId = cameraId;
    session->deviceGbId = deviceGbId;
    session->platformGbId = platformGbId;
    session->cameraDbId = cameraDbId;
    session->status = StreamSessionStatus::INIT;
    session->createTime = std::chrono::steady_clock::now();
    session->lastActivityTime = session->createTime;
    
    // 生成播放URL
    session->flvUrl = generateFlvUrl(session->streamId);
    session->wsFlvUrl = generateWsFlvUrl(session->streamId);
    
    sessions_[session->sessionId] = session;
    streamIdIndex_[session->streamId] = session;
    persistSessionRecord(session);
    
    sessionCounter_++;
    
    std::cout << "[MediaService] Created session " << session->sessionId 
              << " for stream " << session->streamId << std::endl;
    
    return session;
}

std::shared_ptr<StreamSession> MediaService::createSessionWithStreamId(const std::string& streamId,
                                                                       const std::string& cameraId,
                                                                       const std::string& deviceGbId,
                                                                       const std::string& platformGbId,
                                                                       const std::string& cameraDbId) {
    if (streamId.empty()) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(sessionsMutex_);

    auto it = streamIdIndex_.find(streamId);
    if (it != streamIdIndex_.end() && it->second->status != StreamSessionStatus::CLOSED) {
        return it->second;
    }

    auto session = std::make_shared<StreamSession>();
    session->sessionId = generateSessionId();
    session->streamId = streamId;
    session->cameraId = cameraId;
    session->deviceGbId = deviceGbId;
    session->platformGbId = platformGbId;
    session->cameraDbId = cameraDbId;
    session->status = StreamSessionStatus::INIT;
    session->createTime = std::chrono::steady_clock::now();
    session->lastActivityTime = session->createTime;
    session->flvUrl = generateFlvUrl(session->streamId);
    session->wsFlvUrl = generateWsFlvUrl(session->streamId);

    sessions_[session->sessionId] = session;
    streamIdIndex_[session->streamId] = session;
    persistSessionRecord(session);
    sessionCounter_++;
    std::cout << "[MediaService] Created custom stream session " << session->sessionId << " streamId=" << streamId
              << std::endl;
    return session;
}

std::shared_ptr<StreamSession> MediaService::attachToExistingStream(const std::string& cameraId,
                                                                    const std::string& deviceGbId,
                                                                    const std::string& platformGbId,
                                                                    const std::string& cameraDbId) {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    const std::string streamId = buildStreamId(platformGbId, cameraId);

    auto it = streamIdIndex_.find(streamId);
    if (it != streamIdIndex_.end() && it->second->status != StreamSessionStatus::CLOSED) {
        it->second->lastActivityTime = std::chrono::steady_clock::now();
        return it->second;
    }

    auto session = std::make_shared<StreamSession>();
    session->sessionId = generateSessionId();
    session->streamId = streamId;
    session->cameraId = cameraId;
    session->deviceGbId = deviceGbId;
    session->platformGbId = platformGbId;
    session->status = StreamSessionStatus::STREAMING;
    session->isActive = true;
    session->createTime = std::chrono::steady_clock::now();
    session->lastActivityTime = session->createTime;
    session->flvUrl = generateFlvUrl(session->streamId);
    session->wsFlvUrl = generateWsFlvUrl(session->streamId);
    loadPersistedSession(streamId, session);
    if (!cameraDbId.empty()) {
        session->cameraDbId = cameraDbId;
    }
    session->status = StreamSessionStatus::STREAMING;
    session->isActive = true;
    if (session->flvUrl.empty()) {
        session->flvUrl = generateFlvUrl(session->streamId);
    }
    if (session->wsFlvUrl.empty()) {
        session->wsFlvUrl = generateWsFlvUrl(session->streamId);
    }

    sessions_[session->sessionId] = session;
    streamIdIndex_[session->streamId] = session;
    sessionCounter_++;
    persistSessionRecord(session);

    std::cout << "[MediaService] Attached local session " << session->sessionId
              << " to existing stream " << session->streamId << std::endl;

    return session;
}

std::shared_ptr<StreamSession> MediaService::getSession(const std::string& sessionId) {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    
    auto it = sessions_.find(sessionId);
    if (it != sessions_.end()) {
        it->second->lastActivityTime = std::chrono::steady_clock::now();
        return it->second;
    }
    return nullptr;
}

std::shared_ptr<StreamSession> MediaService::getSessionByStreamId(const std::string& streamId) {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    
    auto it = streamIdIndex_.find(streamId);
    if (it != streamIdIndex_.end()) {
        it->second->lastActivityTime = std::chrono::steady_clock::now();
        return it->second;
    }
    return nullptr;
}

bool MediaService::isLocalStreamPlaying(const std::string& streamId) const {
    if (streamId.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    auto it = streamIdIndex_.find(streamId);
    if (it == streamIdIndex_.end() || !it->second) {
        return false;
    }
    return it->second->status == StreamSessionStatus::STREAMING;
}

void MediaService::updateSessionStatus(const std::string& sessionId, StreamSessionStatus status) {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    
    auto it = sessions_.find(sessionId);
    if (it != sessions_.end()) {
        it->second->status = status;
        it->second->lastActivityTime = std::chrono::steady_clock::now();
        persistSessionRecord(it->second);
        
        std::cout << "[MediaService] Session " << sessionId << " status changed to " 
                  << getStreamStatusName(status) << std::endl;
    }
}

void MediaService::setSessionCallId(const std::string& sessionId, const std::string& callId) {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    
    auto it = sessions_.find(sessionId);
    if (it != sessions_.end()) {
        it->second->callId = callId;
        it->second->lastActivityTime = std::chrono::steady_clock::now();
        persistSessionRecord(it->second);
    }
}

void MediaService::markStreamActive(const std::string& streamId) {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    
    auto it = streamIdIndex_.find(streamId);
    if (it != streamIdIndex_.end()) {
        it->second->isActive = true;
        it->second->status = StreamSessionStatus::STREAMING;
        it->second->lastActivityTime = std::chrono::steady_clock::now();
        persistSessionRecord(it->second);
        
        std::cout << "[MediaService] Stream " << streamId << " is now active" << std::endl;
    }
}

bool MediaService::closeSessionInternal(const std::string& sessionId) {
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) {
        return false;
    }
    
    auto session = it->second;
    
    // 关闭ZLM端口
    if (session->zlmPort > 0) {
        closeRtpServer(session->streamId);
    }
    
    // 更新状态
    session->status = StreamSessionStatus::CLOSED;
    session->isActive = false;
    
    // 从索引中移除
    streamIdIndex_.erase(session->streamId);
    sessions_.erase(it);
    markPersistedSessionClosed(session->streamId);
    
    std::cout << "[MediaService] Closed session " << sessionId << std::endl;
    return true;
}

bool MediaService::closeSession(const std::string& sessionId) {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    return closeSessionInternal(sessionId);
}

size_t MediaService::closeSessionsForCamera(const std::string& cameraId) {
    if (cameraId.empty()) {
        return 0;
    }
    std::vector<std::string> sessionIds;
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        for (const auto& pair : sessions_) {
            if (pair.second && pair.second->cameraId == cameraId) {
                sessionIds.push_back(pair.first);
            }
        }
    }
    for (const auto& sessionId : sessionIds) {
        std::shared_ptr<StreamSession> session;
        {
            std::lock_guard<std::mutex> lock(sessionsMutex_);
            auto it = sessions_.find(sessionId);
            if (it != sessions_.end()) {
                session = it->second;
            }
        }
        if (!session) {
            continue;
        }
        if (!session->callId.empty()) {
            sendPlayByeAsync(session->callId, session->deviceGbId, session->cameraId);
        }
        closeSession(sessionId);
    }
    return sessionIds.size();
}

void MediaService::onViewerAttach(const std::string& streamId) {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    
    auto it = streamIdIndex_.find(streamId);
    if (it != streamIdIndex_.end()) {
        it->second->viewerCount++;
        it->second->lastActivityTime = std::chrono::steady_clock::now();
        persistSessionRecord(it->second);
        
        std::cout << "[MediaService] Viewer attached to " << streamId 
                  << ", count=" << it->second->viewerCount << std::endl;
    }
}

void MediaService::onViewerDetach(const std::string& streamId) {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    
    auto it = streamIdIndex_.find(streamId);
    if (it != streamIdIndex_.end()) {
        if (it->second->viewerCount > 0) {
            it->second->viewerCount--;
        }
        it->second->lastActivityTime = std::chrono::steady_clock::now();
        persistSessionRecord(it->second);
        
        std::cout << "[MediaService] Viewer detached from " << streamId 
                  << ", count=" << it->second->viewerCount << std::endl;
    }
}

int MediaService::getViewerCount(const std::string& streamId) {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    
    auto it = streamIdIndex_.find(streamId);
    if (it != streamIdIndex_.end()) {
        return it->second->viewerCount;
    }
    return 0;
}

// ========== Web Hook处理 ==========

// 保留此方法供向后兼容，但建议直接使用handleStreamNoneReader/handleStreamChanged
bool MediaService::handleZlmHook(const std::string& event, 
                                  const std::string& streamId,
                                  const std::string& params) {
    std::cout << "[MediaService] ZLM Hook (deprecated): event=" << event 
              << ", streamId=" << streamId << std::endl;
    
    // 注意：ZLM Hook现在通过URL路径区分，不是通过event字段
    // 请直接调用handleStreamNoneReader或handleStreamChanged
    
    if (event == "on_stream_none_reader" || event.empty()) {
        return handleStreamNoneReader(streamId);
    } else if (event == "on_stream_changed") {
        bool isRegist = (params.find("\"regist\":true") != std::string::npos) ||
                        (params.find("\"regist\":1") != std::string::npos);
        (void)handleStreamChanged(streamId, isRegist);
        return true;
    }
    
    return true;
}

bool MediaService::handleStreamNoneReader(const std::string& streamId) {
    if (streamId.empty()) {
        WarnL << "【handleStreamNoneReader】empty streamId";
        return false;
    }

    std::lock_guard<std::mutex> lock(sessionsMutex_);

    auto it = streamIdIndex_.find(streamId);
    if (it == streamIdIndex_.end()) {
        InfoL << "【handleStreamNoneReader】no local session for streamId=" << streamId;
        return false;
    }

    auto session = it->second;
    const std::string sessionId = session->sessionId;
    const StreamSessionStatus st = session->status;

    /* ZLM 已根据 streamNoneReaderDelayMS 判定无人观看；本地 viewerCount 未与浏览器播放器同步，
     * 若以此拦截会导致永不拆流。拆流逻辑与 handleRtpServerTimeout 对齐：必要时 BYE + closeSessionInternal。 */
    /* BYE 仅入队 sendPlayByeAsync，真正发 UDP 在 PJSIP 线程 processPendingInvites→sendPlayBye。
     * 不再用 INVITING/STREAMING 等细状态卡死：抓包无 BYE 多为 stream 对不上或误判状态导致未入队。 */
    const bool needBye = !session->callId.empty() && !session->deviceGbId.empty() &&
                         st != StreamSessionStatus::CLOSED;

    if (needBye) {
        sendPlayByeAsync(session->callId, session->deviceGbId, session->cameraId);
        InfoL << "【handleStreamNoneReader】SIP BYE queued streamId=" << streamId
              << " sessionId=" << sessionId << " callId=" << session->callId
              << " deviceGb=" << session->deviceGbId << " channel=" << session->cameraId
              << " status=" << getStreamStatusName(st);
    } else {
        WarnL << "【handleStreamNoneReader】skip BYE (no callId/deviceGb or already closed) streamId="
              << streamId << " callId_empty=" << (session->callId.empty() ? "yes" : "no")
              << " status=" << getStreamStatusName(st);
    }

    closeSessionInternal(sessionId);
    InfoL << "【handleStreamNoneReader】session closed streamId=" << streamId << " sessionId=" << sessionId;
    return true;
}

bool MediaService::handleStreamChanged(const std::string& streamId, bool isRegist) {
    std::lock_guard<std::mutex> lock(sessionsMutex_);

    auto it = streamIdIndex_.find(streamId);
    if (it == streamIdIndex_.end()) {
        return false;
    }

    auto session = it->second;

    if (isRegist) {
        // 流注册（开始推流）
        session->isActive = true;
        if (session->status == StreamSessionStatus::INVITING ||
            session->status == StreamSessionStatus::INIT) {
            session->status = StreamSessionStatus::STREAMING;
        }
        persistSessionRecord(session);
        std::cout << "[MediaService] Stream " << streamId << " registered" << std::endl;
        session->lastActivityTime = std::chrono::steady_clock::now();
        return true;
    }

    /* regist=false：BYE 先入队，再 closeSessionInternal。真正发 BYE 见 processPendingInvites→sendPlayBye */
    session->isActive = false;
    std::cout << "[MediaService] Stream " << streamId << " unregistered" << std::endl;

    const std::string sessionId = session->sessionId;
    const std::string callId = session->callId;
    const std::string deviceGbId = session->deviceGbId;
    const std::string cameraId = session->cameraId;
    const StreamSessionStatus st = session->status;

    const bool needBye =
        !callId.empty() && !deviceGbId.empty() && st != StreamSessionStatus::CLOSED;

    if (needBye) {
        sendPlayByeAsync(callId, deviceGbId, cameraId);
        InfoL << "【handleStreamChanged】regist=false SIP BYE queued streamId=" << streamId
              << " sessionId=" << sessionId << " callId=" << callId
              << " status=" << getStreamStatusName(st);
    } else {
        WarnL << "【handleStreamChanged】regist=false skip BYE streamId=" << streamId
              << " callId_empty=" << (callId.empty() ? "yes" : "no") << " status="
              << getStreamStatusName(st);
    }

    closeSessionInternal(sessionId);
    return true;
}

void MediaService::handleRtpServerTimeout(const std::string& streamId) {
    if (streamId.empty()) {
        return;
    }

    /* ZLM 对 openRtpServer 无 RTP 的判定经 on_rtp_server_timeout 回调送达；上级桥接须完整 teardown（含未答上级的 486） */
    if (upstreamBridgeTryTeardownForRtpServerTimeout(streamId)) {
        return;
    }

    std::lock_guard<std::mutex> lock(sessionsMutex_);

    auto it = streamIdIndex_.find(streamId);
    if (it == streamIdIndex_.end()) {
        InfoL << "[MediaService] handleRtpServerTimeout: no local session (already closed or unknown stream_id="
              << streamId << ")";
        return;
    }

    auto session = it->second;
    const StreamSessionStatus st = session->status;
    const std::string sessionId = session->sessionId;

    const bool needBye = !session->callId.empty() &&
                          (st == StreamSessionStatus::INIT || st == StreamSessionStatus::INVITING ||
                           st == StreamSessionStatus::STREAMING);

    if (needBye) {
        session->errorMessage = "preview_rtp_server_timeout";
        persistSessionRecord(session);
        sendPlayByeAsync(session->callId, session->deviceGbId, session->cameraId);
        InfoL << "[MediaService] handleRtpServerTimeout: BYE queued streamId=" << streamId
              << " status=" << getStreamStatusName(st);
    } else if (st == StreamSessionStatus::CLOSING) {
        InfoL << "[MediaService] handleRtpServerTimeout: skip BYE (CLOSING, none_reader path) streamId="
              << streamId;
    }

    closeSessionInternal(sessionId);
}

// ========== 会话清理 ==========

size_t MediaService::cleanupExpiredSessions(int maxAgeSeconds) {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    
    auto now = std::chrono::steady_clock::now();
    std::vector<std::string> toRemove;
    
    for (const auto& pair : sessions_) {
        auto age = std::chrono::duration_cast<std::chrono::seconds>(
            now - pair.second->createTime).count();
        
        // 清理已关闭或过期的会话
        if (pair.second->status == StreamSessionStatus::CLOSED ||
            (pair.second->status != StreamSessionStatus::STREAMING && age > maxAgeSeconds)) {
            toRemove.push_back(pair.first);
        }
    }
    
    for (const auto& sessionId : toRemove) {
        closeSessionInternal(sessionId);
    }
    
    return toRemove.size();
}

size_t MediaService::getActiveSessionCount() const {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    
    size_t count = 0;
    for (const auto& pair : sessions_) {
        if (pair.second->status != StreamSessionStatus::CLOSED) {
            count++;
        }
    }
    return count;
}

} // namespace gb
