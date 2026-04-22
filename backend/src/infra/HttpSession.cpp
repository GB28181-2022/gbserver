/**
 * @file HttpSession.cpp
 * @brief HTTP 会话实现
 * @details RESTful API 接口实现：
 *          - 请求解析（GET/POST/PUT/DELETE）
 *          - 路由分发
 *          - 设备/平台/摄像头 CRUD 操作
 *          - 认证授权
 *          - JSON 响应构造
 *          - 云台 POST /api/ptz → enqueuePtzDeviceControl
 * @date 2025
 * @note 使用原始字符串拼接 JSON，无外部 JSON 库依赖
 */
#include "infra/HttpSession.h"
#include "infra/AuthHelper.h"
#include "infra/CatalogGroupService.h"
#include "infra/DbUtil.h"
#include "infra/SipCatalog.h"
#include "infra/MediaService.h"
#include "infra/SipServerPjsip.h"
#include "infra/UpstreamRegistrar.h"
#include "infra/UpstreamInviteBridge.h"
#include <cstring>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <thread>
#include <fstream>
#include <iterator>
#include <set>
#include <mutex>
#include <unordered_map>

using namespace toolkit;

namespace gb {

namespace {

const char kHealthBody[] = "{\"code\":0,\"message\":\"ok\",\"data\":{\"status\":\"ok\"}}";  /**< 健康检查响应体 */
const size_t kHealthBodyLen = sizeof(kHealthBody) - 1;

/**
 * @brief 布尔值转 JSON 字符串
 * @param v 布尔值
 * @return "true" 或 "false"
 */
std::string jsonBool(bool v) {
  return v ? "true" : "false";
}

/**
 * @brief JSON 字符串转义
 * @param s 原始字符串
 * @return 转义后的字符串（转义 \ 和 "）
 */
std::string escapeJsonString(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '\\') out += "\\\\";
    else if (c == '"') out += "\\\"";
    else out += c;
  }
  return out;
}

std::string trimWhitespace(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

static int jsonHexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/** PostgreSQL encode(convert_to(...,'UTF8'),'hex') 解码；用于列表查询中携带 reg_password 且避免 psql | 分隔符被密码内容破坏 */
static std::string postgresHexFieldDecode(const std::string& hex) {
  std::string h = trimWhitespace(hex);
  if (h.empty()) return "";
  if (h.size() % 2 != 0) return "";
  std::string out;
  out.reserve(h.size() / 2);
  for (size_t i = 0; i + 1 < h.size(); i += 2) {
    int hi = jsonHexNibble(h[i]);
    int lo = jsonHexNibble(h[i + 1]);
    if (hi < 0 || lo < 0) return "";
    out.push_back(static_cast<char>((hi << 4) | lo));
  }
  return out;
}

/**
 * @brief 从平台独立「流媒体地址」解析 SDP 连接用主机名或 IPv4（不含端口）
 */
std::string parseStreamMediaHost(const std::string& raw) {
  std::string u = trimWhitespace(raw);
  if (u.empty()) return "";
  std::string lower = u;
  for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (lower.size() > 8 && lower.compare(0, 8, "https://") == 0)
    u = u.substr(8);
  else if (lower.size() > 7 && lower.compare(0, 7, "http://") == 0)
    u = u.substr(7);
  size_t slash = u.find('/');
  if (slash != std::string::npos) u = u.substr(0, slash);
  size_t at = u.rfind('@');
  if (at != std::string::npos) u = u.substr(at + 1);
  u = trimWhitespace(u);
  if (!u.empty() && u[0] == '[') {
    size_t br = u.find(']');
    if (br != std::string::npos) return trimWhitespace(u.substr(1, br - 1));
  }
  size_t colon = u.find(':');
  if (colon != std::string::npos) return trimWhitespace(u.substr(0, colon));
  return u;
}

bool jsonKeyHasNull(const std::string& body, const char* key) {
  std::string pat = std::string("\"") + key + "\":null";
  return body.find(pat) != std::string::npos;
}

/**
 * @brief 字符串分割
 * @param s 原始字符串
 * @param delim 分隔符
 * @return 分割后的字符串数组
 */
std::vector<std::string> split(const std::string& s, char delim) {
  std::vector<std::string> parts;
  std::string current;
  for (char c : s) {
    if (c == delim) {
      parts.push_back(current);
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  parts.push_back(current);
  return parts;
}

/**
 * @brief 解析点播 SDP 连接 IP 与是否 TCP（平台独立配置优先，否则系统 media_config）
 */
void resolvePreviewStreamParams(const std::string& platformGbId, std::string& outSdpIp, bool& outUseTcp) {
  outUseTcp = false;
  outSdpIp.clear();
  std::string sysHost = "127.0.0.1";
  std::string sysRtp = "udp";
  {
    std::string q = gb::execPsql(
        "SELECT COALESCE(NULLIF(TRIM(media_http_host),''), '127.0.0.1'), "
        "LOWER(COALESCE(NULLIF(TRIM(rtp_transport),''), 'udp')) "
        "FROM media_config WHERE id = 1 LIMIT 1");
    if (!q.empty()) {
      size_t nl = q.find('\n');
      std::string line = (nl == std::string::npos) ? q : q.substr(0, nl);
      std::vector<std::string> cols = split(line, '|');
      if (!cols.empty()) {
        std::string h = trimWhitespace(cols[0]);
        if (!h.empty()) sysHost = h;
      }
      if (cols.size() > 1 && trimWhitespace(cols[1]) == "tcp") sysRtp = "tcp";
    }
  }
  outSdpIp = sysHost;
  outUseTcp = (sysRtp == "tcp");

  std::string platSql = "SELECT strategy_mode, COALESCE(stream_media_url,''), "
                        "LOWER(COALESCE(stream_rtp_transport::text, '')) "
                        "FROM device_platforms WHERE gb_id='" +
                        gb::escapeSqlString(platformGbId) + "' LIMIT 1";
  std::string pq = gb::execPsql(platSql.c_str());
  if (pq.empty()) return;
  size_t nl = pq.find('\n');
  std::string line = (nl == std::string::npos) ? pq : pq.substr(0, nl);
  std::vector<std::string> cols = split(line, '|');
  if (cols.empty()) return;
  std::string strategy = trimWhitespace(cols[0]);
  std::string streamUrl = cols.size() > 1 ? cols[1] : "";
  std::string platRtp = cols.size() > 2 ? trimWhitespace(cols[2]) : "";
  if (strategy != "custom") return;
  std::string hostFromUrl = parseStreamMediaHost(streamUrl);
  if (!hostFromUrl.empty()) outSdpIp = hostFromUrl;
  if (platRtp == "tcp")
    outUseTcp = true;
  else if (platRtp == "udp")
    outUseTcp = false;
}

std::string extractJsonField(const std::string& body, const std::string& key) {
  // 必须匹配完整键名 "key":，避免 "username" 等字段中的子串 "name" 误命中后错取冒号
  const std::string pat = std::string("\"") + key + "\":";
  size_t keyPos = body.find(pat);
  if (keyPos == std::string::npos) return "";

  size_t valueStart = keyPos + pat.size();
  while (valueStart < body.size() &&
         (body[valueStart] == ' ' || body[valueStart] == '\t' ||
          body[valueStart] == '\r' || body[valueStart] == '\n')) {
    ++valueStart;
  }
  if (valueStart >= body.size()) return "";

  if (body[valueStart] == '"') {
    ++valueStart;
    size_t valueEnd = body.find('"', valueStart);
    if (valueEnd == std::string::npos) return "";
    return body.substr(valueStart, valueEnd - valueStart);
  }

  size_t valueEnd = valueStart;
  while (valueEnd < body.size() && body[valueEnd] != ',' && body[valueEnd] != '}') {
    ++valueEnd;
  }
  while (valueEnd > valueStart &&
         (body[valueEnd - 1] == ' ' || body[valueEnd - 1] == '\t' ||
          body[valueEnd - 1] == '\r' || body[valueEnd - 1] == '\n')) {
    --valueEnd;
  }
  return body.substr(valueStart, valueEnd - valueStart);
}

bool isNumericId(const std::string& value) {
  if (value.empty()) return false;
  for (char ch : value) {
    if (ch < '0' || ch > '9') return false;
  }
  return true;
}

bool looksLikeGbId(const std::string& value) {
  return value.size() >= 18;
}

std::string queryPlatformGbIdByPlatformId(const std::string& platformId) {
  if (platformId.empty()) return "";

  std::string sql = "SELECT gb_id FROM device_platforms WHERE id = " + platformId + " LIMIT 1";
  std::string out = gb::execPsql(sql.c_str());
  if (out.empty()) return "";

  size_t nl = out.find('\n');
  std::string result = (nl == std::string::npos) ? out : out.substr(0, nl);
  size_t first = result.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return "";
  result = result.substr(first);
  while (!result.empty() &&
         (result.back() == '\r' || result.back() == '\n' ||
          result.back() == ' ' || result.back() == '\t')) {
    result.pop_back();
  }
  return result;
}

std::string resolvePlatformGbIdFromBody(const std::string& body) {
  const std::string platformGbId = extractJsonField(body, "platformGbId");
  if (!platformGbId.empty()) return platformGbId;

  const std::string platformDbId = extractJsonField(body, "platformDbId");
  if (!platformDbId.empty() && isNumericId(platformDbId)) {
    return queryPlatformGbIdByPlatformId(platformDbId);
  }

  const std::string platformId = extractJsonField(body, "platformId");
  if (platformId.empty()) return "";
  if (looksLikeGbId(platformId)) return platformId;
  if (isNumericId(platformId)) return queryPlatformGbIdByPlatformId(platformId);
  return platformId;
}

/**
 * @brief 解析 HTTP 请求行
 * @param data 请求数据
 * @param len 数据长度
 * @param method 输出：请求方法（GET/POST/PUT/DELETE）
 * @param path 输出：请求路径（不含查询串）
 * @param query 输出：查询字符串（? 之后的部分）
 * @details 解析格式：METHOD PATH?query HTTP/1.1
 */
void parseMethodPath(const char* data, size_t len, std::string& method, std::string& path, std::string& query) {
  method.clear();
  path.clear();
  query.clear();
  const char* end = static_cast<const char*>(std::memchr(data, '\r', len));
  if (!end) end = static_cast<const char*>(std::memchr(data, '\n', len));
  size_t lineLen = end ? static_cast<size_t>(end - data) : len;
  std::string line(data, lineLen);
  size_t s1 = line.find(' ');
  if (s1 == std::string::npos) return;
  method = line.substr(0, s1);
  size_t s2 = line.find(' ', s1 + 1);
  path = (s2 == std::string::npos) ? line.substr(s1 + 1) : line.substr(s1 + 1, s2 - s1 - 1);
  size_t q = path.find('?');
  if (q != std::string::npos) {
    query = path.substr(q + 1);
    path = path.substr(0, q);
  }
}

/**
 * @brief 去除字符串尾部换行和空白
 * @param s 字符串
 */
static void trimR(std::string& s) {
  while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
}

static int hexCharToInt(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  return -1;
}

/**
 * @brief 查询串参数值百分号解码（application/x-www-form-urlencoded：+ 视为空格）
 * @details 前端 URLSearchParams 会对 ISO8601 中的 :、+ 等编码；不解码则 timestamptz 写入失败。
 */
static std::string decodeUrlQueryValue(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '+') {
      out += ' ';
    } else if (s[i] == '%' && i + 2 < s.size()) {
      int hi = hexCharToInt(s[i + 1]);
      int lo = hexCharToInt(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out += static_cast<char>((hi << 4) | lo);
        i += 2;
        continue;
      }
    }
    out += s[i];
  }
  return out;
}

/**
 * @brief 获取 URL 查询参数
 * @param query 查询字符串
 * @param key 参数名
 * @return 参数值，不存在返回空字符串
 * @details 对参数值做百分号解码，与浏览器 fetch/URLSearchParams 行为一致。
 */
std::string getQueryParam(const std::string& query, const char* key) {
  if (query.empty() || !key) return {};
  std::string keyStr(key);
  size_t start = 0;
  while (start < query.size()) {
    size_t eq = query.find('=', start);
    if (eq == std::string::npos) break;
    std::string k = query.substr(start, eq - start);
    size_t next = query.find('&', eq + 1);
    if (next == std::string::npos) next = query.size();
    std::string v = query.substr(eq + 1, next - eq - 1);
    if (k == keyStr) return decodeUrlQueryValue(v);
    start = next + 1;
  }
  return {};
}

/**
 * @brief 获取 HTTP 头部值
 * @param data HTTP 请求数据
 * @param len 数据长度
 * @param key 头部名称（不区分大小写）
 * @return 头部值，不存在返回空字符串
 */
std::string getHeaderValue(const char* data, size_t len, const char* key) {
  std::string keyStr(key);
  std::string block(data, len);
  size_t pos = 0;
  while (pos < block.size()) {
    size_t lineEnd = block.find("\r\n", pos);
    if (lineEnd == std::string::npos) lineEnd = block.size();
    std::string line = block.substr(pos, lineEnd - pos);
    if (line.empty()) break;
    size_t colon = line.find(':');
    if (colon != std::string::npos) {
      std::string k = line.substr(0, colon);
      std::string v = line.substr(colon + 1);
      while (!v.empty() && (v[0] == ' ' || v[0] == '\t')) v.erase(0, 1);
      if (k.size() == keyStr.size()) {
        bool eq = true;
        for (size_t i = 0; i < k.size(); ++i) {
          if (std::tolower(static_cast<unsigned char>(k[i])) != std::tolower(static_cast<unsigned char>(keyStr[i]))) {
            eq = false;
            break;
          }
        }
        if (eq) return v;
      }
    }
    pos = lineEnd + 2;
  }
  return {};
}

/** @brief 单调时钟毫秒，用于对比 ZLM Hook 先后顺序（与系统墙钟无关） */
static uint64_t zlmHookSteadyMs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

bool tryExtractCompleteHttpRequest(std::string& buffer, std::string& request) {
  const std::string sep = "\r\n\r\n";
  size_t headerEnd = buffer.find(sep);
  if (headerEnd == std::string::npos) {
    return false;
  }

  const size_t bodyOffset = headerEnd + sep.size();
  size_t bodyLength = 0;
  std::string cl = getHeaderValue(buffer.data(), headerEnd, "Content-Length");
  if (!cl.empty()) {
    bodyLength = static_cast<size_t>(std::max(0, std::atoi(cl.c_str())));
  }

  const size_t totalLength = bodyOffset + bodyLength;
  if (buffer.size() < totalLength) {
    return false;
  }

  request = buffer.substr(0, totalLength);
  buffer.erase(0, totalLength);
  return true;
}

/**
 * @brief 获取 HTTP Body
 * @param data HTTP 请求数据
 * @param len 数据长度
 * @return Body 内容
 * @details 根据 \r\n\r\n 分隔符或 Content-Length 头部提取 Body
 */
std::string getBody(const char* data, size_t len) {
  const char* sep = "\r\n\r\n";
  const size_t sepLen = 4;
  const char* bodyStart = static_cast<const char*>(std::search(data, data + len, sep, sep + sepLen));
  if (!bodyStart || bodyStart + sepLen > data + len) return {};
  size_t headerLen = static_cast<size_t>(bodyStart - data);
  bodyStart += sepLen;
  size_t bodyLen = static_cast<size_t>((data + len) - bodyStart);
  std::string cl = getHeaderValue(data, headerLen, "Content-Length");
  if (!cl.empty()) {
    int n = std::atoi(cl.c_str());
    if (n > 0 && static_cast<size_t>(n) <= bodyLen) bodyLen = static_cast<size_t>(n);
  }
  return std::string(bodyStart, bodyLen);
}

// 从 JSON 体中简单提取 "key":"value" 的 value（不含转义）
std::string extractJsonString(const std::string& body, const char* key) {
  std::string pattern = std::string("\"") + key + "\":\"";
  size_t p = body.find(pattern);
  if (p == std::string::npos) return {};
  p += pattern.size();
  size_t end = body.find('"', p);
  if (end == std::string::npos) return {};
  return body.substr(p, end - p);
}

// 提取 "key": 后的数字
int extractJsonInt(const std::string& body, const char* key) {
  std::string pattern = std::string("\"") + key + "\":";
  size_t p = body.find(pattern);
  if (p == std::string::npos) return 0;
  p += pattern.size();
  while (p < body.size() && (body[p] == ' ' || body[p] == '\t')) ++p;
  if (p >= body.size()) return 0;
  return std::atoi(body.c_str() + p);
}

// 提取 "key": 后的布尔（true/false），支持可选空白与带引号的 "true"/"false"
bool extractJsonBool(const std::string& body, const char* key) {
  std::string pattern = std::string("\"") + key + "\":";
  size_t p = body.find(pattern);
  if (p == std::string::npos) return false;
  p += pattern.size();
  while (p < body.size() && (body[p] == ' ' || body[p] == '\t')) ++p;
  if (p + 4 <= body.size() && body.compare(p, 4, "true") == 0) return true;
  if (p + 5 <= body.size() && body.compare(p, 5, "false") == 0) return false;
  if (p < body.size() && body[p] == '"') {
    ++p;
    if (p + 4 <= body.size() && body.compare(p, 4, "true") == 0 && p + 4 < body.size() && body[p + 4] == '"')
      return true;
    if (p + 5 <= body.size() && body.compare(p, 5, "false") == 0 && p + 5 < body.size() && body[p + 5] == '"')
      return false;
  }
  return false;
}

static bool upstreamGbIdValid(const std::string& s) {
  if (s.size() != 20) return false;
  for (unsigned char c : s) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

static bool upstreamDomainValid(const std::string& s) {
  if (s.empty() || s.size() > 32) return false;
  for (unsigned char c : s) {
    if (std::isalnum(c)) continue;
    if (c == '.' || c == '-' || c == '_') continue;
    return false;
  }
  return true;
}

/** @param excludeNumericId 非空时排除该行 id（PUT 自身） */
static bool upstreamPlatformGbIdTaken(const std::string& gbId, const char* excludeNumericId) {
  std::string q =
      "SELECT count(*)::text FROM upstream_platforms WHERE gb_id='" + gb::escapeSqlString(gbId) + "'";
  if (excludeNumericId && excludeNumericId[0] != '\0') {
    q += " AND id<>";
    q += excludeNumericId;
  }
  std::string out = gb::execPsql(q.c_str());
  std::string cnt = trimWhitespace(out);
  size_t nl = cnt.find('\n');
  if (nl != std::string::npos) cnt = cnt.substr(0, nl);
  while (!cnt.empty() && (cnt.back() == '\r' || cnt.back() == ' ')) cnt.pop_back();
  return !cnt.empty() && cnt != "0";
}

/**
 * @brief 解析 JSON 数组中的正整数，兼容 [1,2] 与 ["1","2"] 两种写法
 * @param body 原始 JSON 文本
 * @param key 数组字段名（不含引号）
 * @param out 输出
 */
static void parsePositiveIntArrayField(const std::string& body, const char* key, std::vector<long long>& out) {
  out.clear();
  if (!key || key[0] == '\0') return;
  std::string needle = "\"";
  needle += key;
  needle += "\"";
  size_t p = body.find(needle);
  if (p == std::string::npos) return;
  p = body.find('[', p);
  if (p == std::string::npos) return;

  size_t q = p + 1;
  while (q < body.size()) {
    while (q < body.size() &&
           (body[q] == ' ' || body[q] == '\t' || body[q] == '\n' || body[q] == '\r' || body[q] == ',')) {
      ++q;
    }
    if (q >= body.size() || body[q] == ']') break;

    bool quoted = false;
    if (body[q] == '"') {
      quoted = true;
      ++q;
    }

    size_t start = q;
    while (q < body.size() && body[q] >= '0' && body[q] <= '9') ++q;
    if (q > start) {
      long long v = std::atoll(body.substr(start, q - start).c_str());
      if (v > 0) out.push_back(v);
    }

    if (quoted) {
      while (q < body.size() && body[q] != '"') ++q;
      if (q < body.size() && body[q] == '"') ++q;
    }
    while (q < body.size() && body[q] != ',' && body[q] != ']') ++q;
    if (q < body.size() && body[q] == ']') break;
  }
}

static void parseCatalogGroupNodeIdsArray(const std::string& body, std::vector<long long>& out) {
  parsePositiveIntArrayField(body, "catalogGroupNodeIds", out);
}

static void parseExcludedCameraIdsArray(const std::string& body, std::vector<long long>& out) {
  parsePositiveIntArrayField(body, "excludedCameraIds", out);
}

/**
 * @brief 从 body 的 transport 对象中解析 udp/tcp（与 http-contracts 嵌套结构一致）
 * @return 找到合法 transport 对象则为 true（即使内部未写 udp/tcp，也会用默认值）
 */
static bool extractTransportObjectBools(const std::string& body, bool& outUdp, bool& outTcp) {
  size_t tp = body.find("\"transport\"");
  if (tp == std::string::npos) return false;
  size_t colon = body.find(':', tp);
  if (colon == std::string::npos) return false;
  size_t q = colon + 1;
  while (q < body.size() && (body[q] == ' ' || body[q] == '\t')) ++q;
  if (q >= body.size() || body[q] != '{') return false;
  size_t br = q;
  int depth = 0;
  size_t i = br;
  for (; i < body.size(); ++i) {
    if (body[i] == '{')
      ++depth;
    else if (body[i] == '}') {
      --depth;
      if (depth == 0) break;
    }
  }
  if (depth != 0) return false;
  std::string tobj = body.substr(br, i - br + 1);
  outUdp = true;
  outTcp = false;
  if (tobj.find("\"udp\"") != std::string::npos) outUdp = extractJsonBool(tobj, "udp");
  if (tobj.find("\"tcp\"") != std::string::npos) outTcp = extractJsonBool(tobj, "tcp");
  return true;
}

// 提取 "cameraIds": ["id1","id2"] 中的 id 列表到 out（简单解析）
void extractJsonCameraIds(const std::string& body, std::vector<std::string>& out) {
  out.clear();
  size_t p = body.find("\"cameraIds\":");
  if (p == std::string::npos) return;
  p = body.find('[', p);
  if (p == std::string::npos) return;
  ++p;
  while (p < body.size()) {
    while (p < body.size() && (body[p] == ' ' || body[p] == ',' || body[p] == '\t')) ++p;
    if (p >= body.size() || body[p] == ']') break;
    if (body[p] == '"') {
      ++p;
      size_t end = body.find('"', p);
      if (end != std::string::npos && end > p) {
        out.push_back(body.substr(p, end - p));
        p = end + 1;
      } else break;
    } else break;
  }
}

void trimSqlField(std::string& s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
    s.erase(0, 1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
    s.pop_back();
  }
}

/**
 * @brief 根据路径/查询中的摄像头键解析 node_ref、platform_id、device_gb_id、库内 id
 * @param cameraKeyRaw 未 SQL 转义的键（库内 id ≤12 位数字，或 device_gb_id）
 * @param platformGbIdOpt 多平台同号时消歧，可为空
 */
bool fetchCameraCatalogKeys(const std::string& cameraKeyRaw,
                            const std::string& platformGbIdOpt,
                            std::string& outNodeRef,
                            std::string& outPlatformId,
                            std::string& outDeviceGbId,
                            std::string& outDbId) {
  std::string dbId, devGb, platGb;
  if (!gb::resolveCameraRowByPathSegment(cameraKeyRaw, platformGbIdOpt, dbId, devGb, platGb)) {
    return false;
  }
  outDeviceGbId = devGb;
  outDbId = dbId;
  std::string sql = "SELECT COALESCE(node_ref::text,''), COALESCE(platform_id::text,'') FROM cameras WHERE id=" +
                     dbId + " LIMIT 1";
  std::string out = gb::execPsql(sql.c_str());
  if (out.empty()) {
    return false;
  }
  size_t nl = out.find('\n');
  std::string line = (nl == std::string::npos) ? out : out.substr(0, nl);
  trimR(line);
  if (line.empty()) {
    return false;
  }
  std::vector<std::string> cols = split(line, '|');
  if (cols.size() < 2) {
    return false;
  }
  outNodeRef = cols[0];
  outPlatformId = cols[1];
  trimSqlField(outNodeRef);
  trimSqlField(outPlatformId);
  return true;
}

/**
 * @brief 删除 catalog_nodes 中与摄像头对应的设备节点（node_type=0）
 * @param cameraIdEscaped gb::escapeSqlString 后的设备国标 ID
 */
void deleteCatalogNodesForCamera(const std::string& cameraIdEscaped,
                                 const std::string& nodeRefTrimmed,
                                 const std::string& platformIdTrimmed) {
  if (!nodeRefTrimmed.empty() && isNumericId(nodeRefTrimmed)) {
    gb::execPsqlCommand("DELETE FROM catalog_nodes WHERE id=" + nodeRefTrimmed);
    return;
  }
  if (!platformIdTrimmed.empty() && isNumericId(platformIdTrimmed)) {
    gb::execPsqlCommand("DELETE FROM catalog_nodes WHERE platform_id=" + platformIdTrimmed +
                        " AND node_type=0 AND (node_id='" + cameraIdEscaped + "' OR device_id='" +
                        cameraIdEscaped + "')");
    return;
  }
  // platform_id 为空时：仅按国标 ID 匹配设备节点（跨平台若有重复 node_id 会一并删除，边界情况）
  gb::execPsqlCommand("DELETE FROM catalog_nodes WHERE node_type=0 AND (node_id='" + cameraIdEscaped +
                      "' OR device_id='" + cameraIdEscaped + "')");
}

void ensureReplayDownloadFilePathColumn() {
  gb::execPsqlCommand("ALTER TABLE replay_downloads ADD COLUMN IF NOT EXISTS file_path VARCHAR(1024)");
}

/* ---- 下载取消机制 ---- */
std::set<int64_t> g_cancelledDownloads;
std::mutex g_cancelMutex;

bool isDownloadCancelled(int64_t downloadId) {
  std::lock_guard<std::mutex> lock(g_cancelMutex);
  return g_cancelledDownloads.count(downloadId) > 0;
}

void markDownloadCancelled(int64_t downloadId) {
  std::lock_guard<std::mutex> lock(g_cancelMutex);
  g_cancelledDownloads.insert(downloadId);
}

void clearDownloadCancelled(int64_t downloadId) {
  std::lock_guard<std::mutex> lock(g_cancelMutex);
  g_cancelledDownloads.erase(downloadId);
}

/**
 * @brief 更新下载进度到 DB
 */
void updateDownloadProgress(int64_t downloadId, int progress) {
  gb::execPsqlCommand("UPDATE replay_downloads SET progress=" + std::to_string(progress) +
                      ",updated_at=CURRENT_TIMESTAMP WHERE id=" + std::to_string(downloadId));
}

void updateDownloadFailed(int64_t downloadId) {
  gb::execPsqlCommand("UPDATE replay_downloads SET status='failed',progress=0,updated_at=CURRENT_TIMESTAMP WHERE id=" +
                      std::to_string(downloadId));
}

/**
 * @brief 录像高速下载后台任务
 * @details 使用 SIP INVITE s=Download 模式让设备以最快速率推流，
 *          通过 ZLM startRecord 录制为 MP4 文件。
 *          支持通过 g_cancelledDownloads 集合随时取消。
 */
void runReplayDownloadJob(int64_t downloadId, int64_t segmentPk, const std::string& deviceGbId,
                          const std::string& cameraDbId,
                          const std::string& platformGbIdFromDb) {
  InfoL << "【DownloadJob】开始 downloadId=" << downloadId << " segment=" << segmentPk
        << " cameraDbId=" << cameraDbId << " deviceGbId=" << deviceGbId;

  MediaService& mediaSvc = gb::GetMediaService();
  if (!mediaSvc.initialize()) {
    updateDownloadFailed(downloadId);
    return;
  }

  // 查询片段时间范围
  std::string epochSql =
      "SELECT floor(extract(epoch from s.start_time))::bigint, floor(extract(epoch from s.end_time))::bigint, "
      "COALESCE(s.duration_seconds,0) FROM replay_segments s JOIN replay_tasks t ON t.id=s.task_id WHERE s.id=" +
      std::to_string(segmentPk) + " AND t.camera_id=" + cameraDbId + " LIMIT 1";
  std::string epOut = gb::execPsql(epochSql.c_str());
  if (epOut.empty()) {
    updateDownloadFailed(downloadId);
    return;
  }
  size_t p1 = epOut.find('|');
  size_t p2 = epOut.find('|', p1 == std::string::npos ? 0 : p1 + 1);
  std::string e0 = epOut.substr(0, p1);
  std::string e1 = (p1 == std::string::npos) ? "" : epOut.substr(p1 + 1, (p2 == std::string::npos ? epOut.size() : p2) - p1 - 1);
  std::string durs = (p2 == std::string::npos) ? "0" : epOut.substr(p2 + 1);
  size_t nln = durs.find('\n');
  if (nln != std::string::npos) durs = durs.substr(0, nln);
  uint64_t t0 = static_cast<uint64_t>(std::atoll(e0.c_str()));
  uint64_t t1 = static_cast<uint64_t>(std::atoll(e1.c_str()));
  int durSec = std::atoi(durs.c_str());
  if (durSec < 1) durSec = static_cast<int>(t1 > t0 ? static_cast<int>(t1 - t0) : 60);
  if (durSec > 7200) durSec = 7200;

  const std::string streamId =
      MediaService::buildStreamId(platformGbIdFromDb, deviceGbId) + "_dl_" + std::to_string(downloadId);
  auto session =
      mediaSvc.createSessionWithStreamId(streamId, deviceGbId, platformGbIdFromDb, platformGbIdFromDb, cameraDbId);
  if (!session) {
    updateDownloadFailed(downloadId);
    return;
  }

  // 清理 lambda：确保 SIP/ZLM 资源释放（不含 stopMp4Record，由调用方按需调用）
  auto cleanup = [&](bool sendBye) {
    if (sendBye && !session->callId.empty()) {
      gb::sendPlayByeAsync(session->callId, platformGbIdFromDb, deviceGbId);
    }
    mediaSvc.closeRtpServer(streamId);
    mediaSvc.closeSession(session->sessionId);
  };

  std::string sdpConnIp;
  bool previewUseTcp = false;
  resolvePreviewStreamParams(platformGbIdFromDb, sdpConnIp, previewUseTcp);
  const int zlmTcpMode = previewUseTcp ? 1 : 0;
  uint16_t zlmPort = 0;
  if (!mediaSvc.openRtpServer(session->streamId, zlmPort, zlmTcpMode)) {
    mediaSvc.closeSession(session->sessionId);
    updateDownloadFailed(downloadId);
    return;
  }
  session->zlmPort = zlmPort;
  session->status = StreamSessionStatus::INVITING;

  // 使用 s=Download 模式 INVITE，设备以最快速率发送
  std::string callId;
  if (!gb::sendPlayInviteAsync(platformGbIdFromDb, deviceGbId, zlmPort, callId,
                               sdpConnIp, previewUseTcp, t0, t1, /*isDownload=*/true)) {
    mediaSvc.closeRtpServer(streamId);
    mediaSvc.closeSession(session->sessionId);
    updateDownloadFailed(downloadId);
    return;
  }
  session->callId = callId;
  mediaSvc.setSessionCallId(session->sessionId, callId);

  // ---- Phase 1: 等待流注册 (0-10%) ----
  updateDownloadProgress(downloadId, 2);
  int waitMs = 0;
  while (waitMs < 60000 && !mediaSvc.isStreamExistsInZlm(streamId)) {
    if (isDownloadCancelled(downloadId)) {
      InfoL << "【DownloadJob】Phase1 取消 downloadId=" << downloadId;
      cleanup(true);
      clearDownloadCancelled(downloadId);
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    waitMs += 200;
    if ((waitMs % 4000) == 0) {
      updateDownloadProgress(downloadId, std::min(10, 2 + waitMs / 6000));
    }
  }
  if (!mediaSvc.isStreamExistsInZlm(streamId)) {
    InfoL << "【DownloadJob】流注册超时 downloadId=" << downloadId;
    cleanup(true);
    updateDownloadFailed(downloadId);
    return;
  }
  updateDownloadProgress(downloadId, 10);

  // ---- Phase 2: 启动 ZLM MP4 录制 (10-90%) ----
  // 不传 customized_path → ZLM 录制到默认 www/record/ 目录（浏览器可通过 /zlm/ 直接访问）
  if (!mediaSvc.startMp4Record(streamId, "", 36000)) {
    WarnL << "【DownloadJob】startMp4Record 失败 downloadId=" << downloadId;
    cleanup(true);
    updateDownloadFailed(downloadId);
    return;
  }

  InfoL << "【DownloadJob】MP4 录制开始 streamId=" << streamId << " (ZLM 默认目录)";

  // 监控录制进度：设备在 Download 模式下以最快速率发送，流消失即表示发送完毕
  auto recordStart = std::chrono::steady_clock::now();
  // Download 模式下预估完成时间为录像时长的 1/4（设备通常 4-10x 速率发送）
  const int estimatedDurSec = std::max(10, durSec / 4);
  int consecutiveMissCount = 0;

  while (true) {
    if (isDownloadCancelled(downloadId)) {
      InfoL << "【DownloadJob】Phase2 取消 downloadId=" << downloadId;
      mediaSvc.stopMp4Record(streamId);
      cleanup(true);
      // 等待 ZLM 完成文件写入后删除残留录制文件
      std::this_thread::sleep_for(std::chrono::milliseconds(2000));
      mediaSvc.deleteRecordDirectory(streamId);
      clearDownloadCancelled(downloadId);
      return;
    }

    bool streamAlive = mediaSvc.isStreamExistsInZlm(streamId);
    if (!streamAlive) {
      consecutiveMissCount++;
      // 连续 3 次（1.5s）检测到流不存在才认为设备真正发完
      if (consecutiveMissCount >= 3) {
        InfoL << "【DownloadJob】流消失，设备发送完毕 downloadId=" << downloadId;
        break;
      }
    } else {
      consecutiveMissCount = 0;
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - recordStart).count();
    int pct = 10 + std::min(80, static_cast<int>(elapsed * 80 / std::max(1, estimatedDurSec)));
    updateDownloadProgress(downloadId, pct);

    // 超时保护：最长等待录像时长 * 2（即使设备以 1x 速率发送也应完成）
    if (elapsed > durSec * 2 + 60) {
      WarnL << "【DownloadJob】录制超时 downloadId=" << downloadId << " elapsed=" << elapsed;
      break;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

  // ---- Phase 3: 停止录制，通过 ZLM API 定位文件 (90-100%) ----
  updateDownloadProgress(downloadId, 92);
  mediaSvc.stopMp4Record(streamId);
  cleanup(true);

  // 获取当天日期字符串用于 getMp4RecordFile API
  auto now = std::chrono::system_clock::now();
  std::time_t nowT = std::chrono::system_clock::to_time_t(now);
  struct tm tmBuf;
  localtime_r(&nowT, &tmBuf);
  char dateBuf[16];
  snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d",
           tmBuf.tm_year + 1900, tmBuf.tm_mon + 1, tmBuf.tm_mday);
  std::string dateStr(dateBuf);

  InfoL << "【DownloadJob】通过 ZLM API 查询录制文件 streamId=" << streamId << " date=" << dateStr;
  updateDownloadProgress(downloadId, 95);

  // 通过 ZLM getMp4RecordFile API 查找录制文件（重试最多 10 次）
  std::string rootPath;
  std::vector<std::string> mp4Files;
  bool found = false;
  for (int attempt = 0; attempt < 10; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    rootPath.clear();
    mp4Files.clear();
    if (mediaSvc.getMp4RecordFile(streamId, dateStr, rootPath, mp4Files) && !mp4Files.empty()) {
      InfoL << "【DownloadJob】第 " << (attempt + 1)
            << " 次 API 查询找到 " << mp4Files.size() << " 个文件"
            << " rootPath=" << rootPath;
      found = true;
      break;
    }
    InfoL << "【DownloadJob】第 " << (attempt + 1) << " 次 API 查询未找到文件，继续等待...";
  }

  if (!found || mp4Files.empty()) {
    WarnL << "【DownloadJob】10 次 API 查询均未找到 MP4 downloadId=" << downloadId;
    mediaSvc.deleteRecordDirectory(streamId);
    updateDownloadFailed(downloadId);
    return;
  }

  // 从 rootPath 提取 /www/ 之后的 web 路径
  // rootPath 示例: /home/.../www/record/rtp/{streamId}/{date}/
  // web 路径:      /record/rtp/{streamId}/{date}/
  std::string webDir;
  size_t wwwPos = rootPath.find("/www/");
  if (wwwPos != std::string::npos) {
    webDir = rootPath.substr(wwwPos + 4);  // 跳过 "/www"
  } else {
    webDir = "/record/rtp/" + streamId + "/" + dateStr + "/";
  }
  // 确保 webDir 以 / 结尾
  if (!webDir.empty() && webDir.back() != '/') webDir += '/';

  // 取最后一个文件（最新的录制）
  std::string mp4FileName = mp4Files.back();
  std::string mediaIp = mediaSvc.getMediaServerIp();
  std::string downloadUrl = "http://" + mediaIp + "/zlm" + webDir + mp4FileName;

  InfoL << "【DownloadJob】下载就绪 downloadId=" << downloadId
        << " url=" << downloadUrl;

  // file_path 列存 "streamId::dateStr" 供后续 cleanup 调 deleteRecordDirectory 使用
  // 注意：不能用 '|' 作分隔符，因为 psql 输出也用 '|' 分列，会导致列错位
  std::string cleanupKey = streamId + "::" + dateStr;

  gb::execPsqlCommand("ALTER TABLE replay_downloads ADD COLUMN IF NOT EXISTS download_url VARCHAR(2048)");
  gb::execPsqlCommand(
      "UPDATE replay_downloads SET status='ready',progress=100,"
      "file_path='" + gb::escapeSqlString(cleanupKey) + "',"
      "download_url='" + gb::escapeSqlString(downloadUrl) + "',"
      "updated_at=CURRENT_TIMESTAMP WHERE id=" + std::to_string(downloadId));

  clearDownloadCancelled(downloadId);
}

}  // namespace

/**
 * @brief 构造函数
 * @param sock Socket 连接
 */
HttpSession::HttpSession(const toolkit::Socket::Ptr& sock)
    : toolkit::Session(sock) {
  InfoL << "HttpSession created " << getIdentifier();
}

bool HttpSession::isGetApiHealth(const char* data, size_t len) {
  if (len < 14) return false;
  if (std::strncmp(data, "GET ", 4) != 0) return false;
  const char* end = static_cast<const char*>(std::memchr(data, '\r', len));
  if (!end) end = static_cast<const char*>(std::memchr(data, '\n', len));
  size_t line_len = end ? static_cast<size_t>(end - data) : len;
  if (line_len < 14) return false;
  return std::strstr(data, "/api/health") != nullptr;
}

void HttpSession::handleRequest(const char* data, size_t len) {
  std::string method, path, query;
  parseMethodPath(data, len, method, path, query);

  InfoL << "HTTP " << method << " " << path;

  if (path.find("/api/catalog-group/") == 0) {
    std::string bodyCg;
    if (method == "POST" || method == "PUT") {
      bodyCg = getBody(data, len);
    }
    auto r = gb::dispatchCatalogGroupRequest(method, path, query, bodyCg);
    sendHttpJson(r.httpStatus, r.jsonBody);
    return;
  }

  if (method == "GET") {
    if (path == "/api/health") {
      sendHealthResponse();
      return;
    }
    if (path == "/api/config/local-gb") {
      sendConfigLocalGb();
      return;
    }
    if (path == "/api/config/media") {
      sendConfigMedia();
      return;
    }
    if (path == "/api/platforms") {
      sendPlatforms(query);
      return;
    }
    {
      static constexpr size_t kApiPlatformsPrefixLen = 15;
      if (path.find("/api/platforms/") == 0 && path.size() > kApiPlatformsPrefixLen) {
        std::string tail = path.substr(kApiPlatformsPrefixLen);
        static constexpr char kScopeSuf[] = "/catalog-scope";
        constexpr size_t kScopeLen = sizeof(kScopeSuf) - 1;
        if (tail.size() > kScopeLen && tail.compare(tail.size() - kScopeLen, kScopeLen, kScopeSuf) == 0) {
          std::string idStr = tail.substr(0, tail.size() - kScopeLen);
          if (idStr.find('/') == std::string::npos) {
            sendPlatformCatalogScope(idStr);
            return;
          }
        }
        if (tail.find('/') == std::string::npos) {
          sendPlatformById(tail);
          return;
        }
      }
    }
    if (path.find("/api/platforms") == 0) {
      sendPlatforms(query);
      return;
    }
    if (path == "/api/device-platforms") {
      sendDevicePlatforms(query);
      return;
    }
    if (path == "/api/cameras") {
      sendCameras(query);
      return;
    }
    // GET /api/cameras/{cameraId}/preview/session?platformGbId=...
    {
      static constexpr const char kPrevSessionSuf[] = "/preview/session";
      constexpr size_t kPrevSessionSufLen = sizeof(kPrevSessionSuf) - 1;
      if (path.find("/api/cameras/") == 0 && path.size() > 13 + kPrevSessionSufLen &&
          path.rfind(kPrevSessionSuf) == path.size() - kPrevSessionSufLen) {
        std::string prevCamId = path.substr(13, path.size() - 13 - kPrevSessionSufLen);
        sendPreviewSessionStatus(prevCamId, query);
        return;
      }
    }
    if (path == "/api/catalog/nodes") {
      sendCatalogNodes(query);
      return;
    }
    if (path == "/api/catalog/tree") {
      sendPlatformCatalogTree(query);
      return;
    }
    if (path.size() > 21 && path.find("/api/catalog/nodes/") == 0 && path.rfind("/cameras") == path.size() - 8) {
      std::string nodeId = path.substr(20, path.size() - 28);
      sendCatalogNodeCameras(nodeId);
      return;
    }
    if (path == "/api/auth/me") {
      std::string auth = getHeaderValue(data, len, "Authorization");
      handleAuthMe(auth);
      return;
    }
    if (path == "/api/alarms") {
      sendAlarms(query);
      return;
    }
    if (path == "/api/overview") {
      sendOverview();
      return;
    }
    if (path == "/api/replay/segments") {
      sendReplaySegments(query);
      return;
    }
    {
      static constexpr const char kRepSess[] = "/replay/session";
      constexpr size_t kRepSessLen = sizeof(kRepSess) - 1;
      if (path.find("/api/cameras/") == 0 && path.size() > 13 + kRepSessLen &&
          path.rfind(kRepSess) == path.size() - kRepSessLen) {
        std::string cam = path.substr(13, path.size() - 13 - kRepSessLen);
        sendReplaySessionStatus(cam, query);
        return;
      }
    }
    if (path.find("/api/replay/download/") == 0 && path.size() > 21) {
      std::string rest = path.substr(21);
      if (rest.size() > 5 && rest.rfind("/file") == rest.size() - 5) {
        sendReplayDownloadFile(rest.substr(0, rest.size() - 5));
        return;
      }
      if (rest.find('/') == std::string::npos) {
        sendReplayDownloadStatus(rest);
        return;
      }
    }
  }

  if (method == "PUT") {
    std::string body = getBody(data, len);
    if (path == "/api/config/local-gb") {
      handlePutConfigLocalGb(body);
      return;
    }
    if (path == "/api/config/media") {
      handlePutConfigMedia(body);
      return;
    }
    // 前缀 "/api/platforms/" 长度为 15（此前误用 14 导致 tail 以 '/' 开头，id 被截成空串，PUT/DELETE 不生效）
    static constexpr size_t kApiPlatformsPrefixLen = 15;
    if (path.find("/api/platforms/") == 0 && path.size() > kApiPlatformsPrefixLen) {
      std::string tail = path.substr(kApiPlatformsPrefixLen);
      static constexpr char kScopeSuf[] = "/catalog-scope";
      constexpr size_t kScopeLen = sizeof(kScopeSuf) - 1;
      if (tail.size() > kScopeLen && tail.compare(tail.size() - kScopeLen, kScopeLen, kScopeSuf) == 0) {
        std::string idStr = tail.substr(0, tail.size() - kScopeLen);
        if (idStr.find('/') == std::string::npos) {
          handlePutPlatformCatalogScope(idStr, body);
          return;
        }
      }
      std::string idStr = tail;
      size_t slash = idStr.find('/');
      if (slash != std::string::npos) idStr = idStr.substr(0, slash);
      handlePutPlatform(idStr, body);
      return;
    }
    if (path.find("/api/device-platforms/") == 0 && path.size() > 22) {
      std::string idStr = path.substr(22);
      size_t slash = idStr.find('/');
      if (slash != std::string::npos) idStr = idStr.substr(0, slash);
      handlePutDevicePlatform(idStr, body);
      return;
    }
    if (path.find("/api/catalog/nodes/") == 0 && path.size() > 20) {
      std::string rest = path.substr(20);
      size_t cam = rest.find("/cameras");
      if (cam != std::string::npos && cam + 8 == rest.size()) {
        std::string idStr = rest.substr(0, cam);
        handlePutCatalogNodeCameras(idStr, body);
      } else if (cam == std::string::npos) {
        handlePutCatalogNode(rest, body);
      }
      return;
    }
    if (path.find("/api/alarms/") == 0 && path.size() > 12) {
      std::string idStr = path.substr(12);
      size_t slash = idStr.find('/');
      if (slash != std::string::npos) idStr = idStr.substr(0, slash);
      handlePutAlarm(idStr, body);
      return;
    }
  }

  if (method == "POST") {
    std::string body = getBody(data, len);
    if (path == "/api/auth/login") {
      handleAuthLogin(body);
      return;
    }
    if (path == "/api/auth/logout") {
      std::string auth = getHeaderValue(data, len, "Authorization");
      handleAuthLogout(auth);
      return;
    }
    if (path == "/api/auth/change-password") {
      std::string auth = getHeaderValue(data, len, "Authorization");
      handleAuthChangePassword(body, auth);
      return;
    }
    if (path == "/api/platforms") {
      handlePostPlatform(body);
      return;
    }
    {
      constexpr size_t kPlatPrefLen = 15;  // strlen("/api/platforms/")
      static constexpr char kNotifySuf[] = "/catalog-notify";
      constexpr size_t kNotifyLen = sizeof(kNotifySuf) - 1;
      if (path.find("/api/platforms/") == 0 && path.size() > kPlatPrefLen + kNotifyLen &&
          path.compare(path.size() - kNotifyLen, kNotifyLen, kNotifySuf) == 0) {
        std::string idStr = path.substr(kPlatPrefLen, path.size() - kPlatPrefLen - kNotifyLen);
        if (idStr.find('/') == std::string::npos) {
          handlePostPlatformCatalogNotify(idStr);
          return;
        }
      }
    }
    if (path == "/api/catalog/nodes") {
      handlePostCatalogNode(body);
      return;
    }
    if (path == "/api/device-platforms") {
      handlePostDevicePlatform(body);
      return;
    }
    if (path == "/api/alarms") {
      handlePostAlarm(body);
      return;
    }
    if (path.find("/api/device-platforms/") == 0 && path.rfind("/catalog-query") != std::string::npos) {
      // POST /api/device-platforms/{id}/catalog-query
      size_t start = strlen("/api/device-platforms/");
      size_t end = path.rfind("/catalog-query");
      std::string platformId = path.substr(start, end - start);
      handleCatalogQuery(platformId);
      return;
    }
    // 视频预览（点播）API
    if (path.find("/api/cameras/") == 0 && path.rfind("/preview/start") != std::string::npos) {
      // POST /api/cameras/{id}/preview/start
      size_t start = strlen("/api/cameras/");
      size_t end = path.rfind("/preview/start");
      std::string cameraId = path.substr(start, end - start);
      handlePreviewStart(cameraId, body);
      return;
    }
    if (path.find("/api/cameras/") == 0 && path.rfind("/preview/stop") != std::string::npos) {
      // POST /api/cameras/{id}/preview/stop
      size_t start = strlen("/api/cameras/");
      size_t end = path.rfind("/preview/stop");
      std::string cameraId = path.substr(start, end - start);
      handlePreviewStop(cameraId, body);
      return;
    }
    if (path.find("/api/cameras/") == 0 && path.rfind("/replay/start") != std::string::npos) {
      size_t start = strlen("/api/cameras/");
      size_t end = path.rfind("/replay/start");
      std::string cameraId = path.substr(start, end - start);
      handleReplayStart(cameraId, body);
      return;
    }
    if (path.find("/api/cameras/") == 0 && path.rfind("/replay/stop") != std::string::npos) {
      size_t start = strlen("/api/cameras/");
      size_t end = path.rfind("/replay/stop");
      std::string cameraId = path.substr(start, end - start);
      handleReplayStop(cameraId, body);
      return;
    }
    if (path.find("/api/cameras/") == 0 && path.rfind("/replay/speed") != std::string::npos) {
      size_t start = strlen("/api/cameras/");
      size_t end = path.rfind("/replay/speed");
      std::string cameraId = path.substr(start, end - start);
      handleReplaySpeed(cameraId, body);
      return;
    }
    if (path.find("/api/replay/download/") == 0 && path.rfind("/cancel") != std::string::npos) {
      std::string rest = path.substr(21);
      size_t slashPos = rest.find('/');
      if (slashPos != std::string::npos) {
        std::string idStr = rest.substr(0, slashPos);
        handleReplayDownloadCancel(idStr);
        return;
      }
    }
    if (path.find("/api/replay/download/") == 0 && path.rfind("/cleanup") != std::string::npos) {
      std::string rest = path.substr(21);
      size_t slashPos = rest.find('/');
      if (slashPos != std::string::npos) {
        std::string idStr = rest.substr(0, slashPos);
        handleReplayDownloadCleanup(idStr);
        return;
      }
    }
    if (path == "/api/replay/download") {
      handleReplayDownloadPost(body);
      return;
    }
    if (path == "/api/cameras/batch-delete") {
      handleBatchDeleteCameras(body);
      return;
    }
    {
      static constexpr const char kDataClearSuf[] = "/data/clear";
      constexpr size_t kDataClearSufLen = sizeof(kDataClearSuf) - 1;
      constexpr size_t kCamApiPrefixLen = 13;  // strlen("/api/cameras/")
      if (path.find("/api/cameras/") == 0 && path.size() > kCamApiPrefixLen + kDataClearSufLen &&
          path.rfind(kDataClearSuf) == path.size() - kDataClearSufLen) {
        std::string cam = path.substr(kCamApiPrefixLen, path.size() - kCamApiPrefixLen - kDataClearSufLen);
        handleClearCameraRelatedData(cam);
        return;
      }
    }
    if (path == "/api/ptz") {
      handlePtzControl(body);
      return;
    }
    // ZLM Web Hook - 通过URL路径区分事件类型（不是通过JSON中的event字段）
    if (path == "/api/zlm/hook/on_stream_none_reader") {
      handleZlmHookNoneReader(body);
      return;
    }
    if (path == "/api/zlm/hook/on_stream_changed") {
      handleZlmHookStreamChanged(body);
      return;
    }
    if (path == "/api/zlm/hook/on_rtp_server_timeout") {
      handleZlmHookRtpServerTimeout(body);
      return;
    }
    if (path == "/api/zlm/hook/on_send_rtp_stopped") {
      handleZlmHookSendRtpStopped(body);
      return;
    }
  }

  if (method == "DELETE") {
    if (path.find("/api/cameras/") == 0 && path.size() > 14) {
      std::string rest = path.substr(strlen("/api/cameras/"));
      if (rest.find('/') == std::string::npos) {
        handleDeleteCamera(rest);
        return;
      }
    }
    static constexpr size_t kApiPlatformsPrefixLenDel = 15;
    if (path.find("/api/platforms/") == 0 && path.size() > kApiPlatformsPrefixLenDel) {
      std::string idStr = path.substr(kApiPlatformsPrefixLenDel);
      size_t slash = idStr.find('/');
      if (slash != std::string::npos) idStr = idStr.substr(0, slash);
      handleDeletePlatform(idStr);
      return;
    }
    if (path.find("/api/device-platforms/") == 0 && path.size() > 22) {
      std::string idStr = path.substr(22);
      size_t slash = idStr.find('/');
      if (slash != std::string::npos) idStr = idStr.substr(0, slash);
      handleDeleteDevicePlatform(idStr);
      return;
    }
    if (path.find("/api/catalog/nodes/") == 0 && path.size() > 20 && path.find("/cameras") == std::string::npos) {
      std::string idStr = path.substr(20);
      handleDeleteCatalogNode(idStr);
      return;
    }
  }

  sendNotFound();
}

void HttpSession::sendHttpJson(int httpStatus, const std::string& body) {
  const char* line = "HTTP/1.1 200 OK\r\n";
  switch (httpStatus) {
    case 200:
      line = "HTTP/1.1 200 OK\r\n";
      break;
    case 400:
      line = "HTTP/1.1 400 Bad Request\r\n";
      break;
    case 404:
      line = "HTTP/1.1 404 Not Found\r\n";
      break;
    case 405:
      line = "HTTP/1.1 405 Method Not Allowed\r\n";
      break;
    case 409:
      line = "HTTP/1.1 409 Conflict\r\n";
      break;
    case 500:
      line = "HTTP/1.1 500 Internal Server Error\r\n";
      break;
    default:
      line = "HTTP/1.1 200 OK\r\n";
      break;
  }
  std::string response;
  response += line;
  response += "Content-Type: application/json; charset=utf-8\r\n";
  response += "Content-Length: ";
  response += std::to_string(body.size());
  response += "\r\nConnection: close\r\n\r\n";
  response += body;
  auto buf = toolkit::BufferRaw::create(response.size());
  buf->assign(response.data(), response.size());
  send(buf);
}

void HttpSession::sendJson(const std::string& body) {
  std::string response;
  response += "HTTP/1.1 200 OK\r\n";
  response += "Content-Type: application/json; charset=utf-8\r\n";
  response += "Content-Length: ";
  response += std::to_string(body.size());
  response += "\r\nConnection: close\r\n\r\n";
  response += body;

  auto buf = toolkit::BufferRaw::create(response.size());
  buf->assign(response.data(), response.size());
  send(buf);
}

void HttpSession::sendJsonError(int code, const std::string& message) {
  std::string body = "{\"code\":";
  body += std::to_string(code);
  body += ",\"message\":\"";
  for (char c : message) {
    if (c == '"' || c == '\\') body += '\\';
    body += c;
  }
  body += "\",\"data\":null}";
  std::string response;
  if (code == 401) {
    response += "HTTP/1.1 401 Unauthorized\r\n";
  } else if (code == 400) {
    response += "HTTP/1.1 400 Bad Request\r\n";
  } else if (code == 404) {
    response += "HTTP/1.1 404 Not Found\r\n";
  } else if (code == 409) {
    response += "HTTP/1.1 409 Conflict\r\n";
  } else if (code == 410) {
    response += "HTTP/1.1 410 Gone\r\n";
  } else if (code == 500) {
    response += "HTTP/1.1 500 Internal Server Error\r\n";
  } else if (code == 502) {
    response += "HTTP/1.1 502 Bad Gateway\r\n";
  } else if (code == 503) {
    response += "HTTP/1.1 503 Service Unavailable\r\n";
  } else {
    response += "HTTP/1.1 200 OK\r\n";
  }
  response += "Content-Type: application/json; charset=utf-8\r\n";
  response += "Content-Length: ";
  response += std::to_string(body.size());
  response += "\r\nConnection: close\r\n\r\n";
  response += body;
  auto buf = toolkit::BufferRaw::create(response.size());
  buf->assign(response.data(), response.size());
  send(buf);
}

void HttpSession::sendFileDownload(const std::string& absolutePath, const std::string& downloadFileName) {
  std::ifstream f(absolutePath, std::ios::binary);
  if (!f) {
    sendNotFound();
    return;
  }
  std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  std::string safeName = downloadFileName;
  for (char& c : safeName) {
    if (c < 32 || c == '"' || c == '\\' || c == '/') c = '_';
  }
  std::string header;
  header += "HTTP/1.1 200 OK\r\n";
  header += "Content-Type: application/octet-stream\r\n";
  header += "Content-Disposition: attachment; filename=\"" + safeName + "\"\r\n";
  header += "Content-Length: " + std::to_string(content.size()) + "\r\n";
  header += "Connection: close\r\n\r\n";
  std::string full = header + content;
  auto buf = toolkit::BufferRaw::create(full.size());
  buf->assign(full.data(), full.size());
  send(buf);
}

void HttpSession::sendHealthResponse() {
  sendJson(std::string(kHealthBody, kHealthBodyLen));
}

void HttpSession::sendConfigLocalGb() {
  std::string out = gb::execPsql(
      "SELECT gb_id, domain, name, username, password, signal_ip, signal_port "
      "FROM gb_local_config WHERE id = 1");
  if (out.empty()) {
    // 退回占位数据
    const char fallback[] =
        "{\"code\":0,\"message\":\"ok\",\"data\":{"
        "\"gbId\":\"\",\"domain\":\"\",\"name\":\"\",\"username\":\"\",\"password\":\"\","
        "\"transport\":{\"udp\":true,\"tcp\":false}"
        "}}";
    sendJson(std::string(fallback));
    return;
  }

  // 只取首行
  auto pos = out.find('\n');
  std::string firstLine = (pos == std::string::npos) ? out : out.substr(0, pos);
  std::vector<std::string> cols = split(firstLine, '|');

  std::string gbId = cols.size() > 0 ? cols[0] : "";
  std::string domain = cols.size() > 1 ? cols[1] : "";
  std::string name = cols.size() > 2 ? cols[2] : "";
  std::string username = cols.size() > 3 ? cols[3] : "";
  std::string password = cols.size() > 4 ? cols[4] : "";
  std::string signalIp = cols.size() > 5 ? cols[5] : "";
  int signalPort = 0;
  if (cols.size() > 6 && !cols[6].empty())
    signalPort = std::atoi(cols[6].c_str());
  /* transport_* 单独查询：password 等字段若含 '|' 会破坏单列 psql 切分，导致 tcp/udp 列错位 */
  bool transportUdp = true;
  bool transportTcp = false;
  std::string flags = gb::execPsql(
      "SELECT COALESCE(transport_udp::text,'f'), COALESCE(transport_tcp::text,'f') "
      "FROM gb_local_config WHERE id = 1 LIMIT 1");
  if (!flags.empty()) {
    size_t nlp = flags.find('\n');
    std::string fl = (nlp == std::string::npos) ? flags : flags.substr(0, nlp);
    trimSqlField(fl);
    std::vector<std::string> fp = split(fl, '|');
    if (fp.size() >= 2) {
      trimSqlField(fp[0]);
      trimSqlField(fp[1]);
      transportUdp = (fp[0] == "t" || fp[0] == "true");
      transportTcp = (fp[1] == "t" || fp[1] == "true");
    }
  }

  std::string body = "{\"code\":0,\"message\":\"ok\",\"data\":{";
  body += "\"gbId\":\"" + escapeJsonString(gbId) + "\",";
  body += "\"domain\":\"" + escapeJsonString(domain) + "\",";
  body += "\"name\":\"" + escapeJsonString(name) + "\",";
  body += "\"username\":\"" + escapeJsonString(username) + "\",";
  body += "\"password\":\"" + escapeJsonString(password) + "\",";
  body += "\"signalIp\":\"" + escapeJsonString(signalIp) + "\",";
  body += "\"signalPort\":" + std::to_string(signalPort > 0 ? signalPort : 5060) + ",";
  body += "\"transport\":{";
  body += "\"udp\":" + jsonBool(transportUdp) + ",";
  body += "\"tcp\":" + jsonBool(transportTcp);
  body += "}}}";

  sendJson(body);
}

void HttpSession::sendConfigMedia() {
  gb::MediaService::ensureMediaApiUrlMigration();
  std::string out = gb::execPsql(
      "SELECT rtp_port_start, rtp_port_end, COALESCE(TRIM(media_http_host::text), ''), "
      "COALESCE(TRIM(media_api_url::text), ''), zlm_secret, "
      "COALESCE(NULLIF(TRIM(rtp_transport), ''), 'udp'), "
      "COALESCE(preview_invite_timeout_sec, 45) "
      "FROM media_config WHERE id = 1");
  if (out.empty()) {
    const char fallback[] =
        "{\"code\":0,\"message\":\"ok\",\"data\":{"
        "\"rtpPortRange\":{\"start\":30000,\"end\":30500},"
        "\"playbackHost\":\"127.0.0.1\","
        "\"mediaApiUrl\":\"http://127.0.0.1:880\","
        "\"zlmApiSecretConfigured\":false,"
        "\"rtpTransport\":\"udp\","
        "\"previewInviteTimeoutSec\":45"
        "}}";
    sendJson(std::string(fallback));
    return;
  }

  int start = 30000;
  int end = 30500;
  std::string playbackHost = "127.0.0.1";
  std::string mediaApiUrl = "http://127.0.0.1:880";

  auto pos = out.find('\n');
  std::string firstLine = (pos == std::string::npos) ? out : out.substr(0, pos);
  std::vector<std::string> cols = split(firstLine, '|');

  if (cols.size() > 0 && !cols[0].empty()) {
    start = std::atoi(cols[0].c_str());
  }
  if (cols.size() > 1 && !cols[1].empty()) {
    end = std::atoi(cols[1].c_str());
  }
  if (cols.size() > 2 && !cols[2].empty()) {
    playbackHost = cols[2];
  }
  if (cols.size() > 3 && !cols[3].empty()) {
    mediaApiUrl = cols[3];
  }

  bool secretConfigured = false;
  if (cols.size() > 4) {
    std::string t = cols[4];
    size_t a = t.find_first_not_of(" \t\r\n");
    if (a != std::string::npos) {
      size_t b = t.find_last_not_of(" \t\r\n");
      secretConfigured = (b >= a) && !t.substr(a, b - a + 1).empty();
    }
  }

  std::string rtpTransport = "udp";
  if (cols.size() > 5 && !cols[5].empty()) {
    std::string rt = trimWhitespace(cols[5]);
    if (rt == "tcp") rtpTransport = "tcp";
    else rtpTransport = "udp";
  }

  int previewInviteTimeoutSec = 45;
  if (cols.size() > 6 && !cols[6].empty()) {
    int t = std::atoi(cols[6].c_str());
    if (t > 0) {
      previewInviteTimeoutSec = t;
    }
  }

  std::string body = "{\"code\":0,\"message\":\"ok\",\"data\":{";
  body += "\"rtpPortRange\":{\"start\":" + std::to_string(start) + ",\"end\":" + std::to_string(end) + "},";
  body += "\"playbackHost\":\"" + escapeJsonString(playbackHost) + "\",";
  body += "\"mediaApiUrl\":\"" + escapeJsonString(mediaApiUrl) + "\",";
  body += "\"zlmApiSecretConfigured\":" + jsonBool(secretConfigured) + ",";
  body += "\"rtpTransport\":\"" + rtpTransport + "\",";
  body += "\"previewInviteTimeoutSec\":" + std::to_string(previewInviteTimeoutSec);
  body += "}}";

  sendJson(body);
}

void HttpSession::sendPlatforms(const std::string& query) {
  bool includeScopeDetails = false;
  std::string includeScope = getQueryParam(query, "includeScope");
  if (includeScope == "1" || includeScope == "true") includeScopeDetails = true;
  std::string out = gb::execPsql(
      "SELECT u.id, u.name, u.sip_domain, u.gb_id, u.sip_ip, u.sip_port, u.transport, u.reg_username, "
      "encode(convert_to(COALESCE(u.reg_password,''), 'UTF8'), 'hex'), "
      "COALESCE(u.register_expires,3600), u.enabled, COALESCE(u.heartbeat_interval,60), u.online, "
      "COALESCE(to_char(u.last_heartbeat_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '') "
      "FROM upstream_platforms u ORDER BY u.id ASC");

  std::unordered_map<std::string, std::string> scopeByPlatform;
  std::unordered_map<std::string, int> scopeCountByPlatform;
  {
    std::string sc = gb::execPsql(
        "SELECT upstream_platform_id::text, "
        "COALESCE(string_agg(catalog_group_node_id::text, ',' ORDER BY catalog_group_node_id), '') "
        "FROM upstream_catalog_scope GROUP BY upstream_platform_id");
    if (!trimWhitespace(sc).empty()) {
      for (const auto& raw : split(sc, '\n')) {
        if (raw.empty()) continue;
        std::vector<std::string> scCols = split(raw, '|');
        if (scCols.size() >= 2 && !scCols[0].empty()) {
          scopeByPlatform[scCols[0]] = scCols[1];
          int cnt = 0;
          if (!trimWhitespace(scCols[1]).empty()) {
            cnt = 1;
            for (char ch : scCols[1]) {
              if (ch == ',') ++cnt;
            }
          }
          scopeCountByPlatform[scCols[0]] = cnt;
        }
      }
    }
  }

  std::unordered_map<std::string, std::string> excludeCamByPlatform;
  std::unordered_map<std::string, int> excludeCountByPlatform;
  {
    std::string ex = gb::execPsql(
        "SELECT upstream_platform_id::text, "
        "COALESCE(string_agg(camera_id::text, ',' ORDER BY camera_id), '') "
        "FROM upstream_catalog_camera_exclude GROUP BY upstream_platform_id");
    if (!trimWhitespace(ex).empty()) {
      for (const auto& raw : split(ex, '\n')) {
        if (raw.empty()) continue;
        std::vector<std::string> exCols = split(raw, '|');
        if (exCols.size() >= 2 && !exCols[0].empty()) {
          excludeCamByPlatform[exCols[0]] = exCols[1];
          int cnt = 0;
          if (!trimWhitespace(exCols[1]).empty()) {
            cnt = 1;
            for (char ch : exCols[1]) {
              if (ch == ',') ++cnt;
            }
          }
          excludeCountByPlatform[exCols[0]] = cnt;
        }
      }
    }
  }

  if (out.empty()) {
    // 主查询失败时 execPsql 也会返回空，易与「表内无行」混淆；用 count 区分并给出明确错误
    std::string cntRaw = gb::execPsql("SELECT count(*)::text FROM upstream_platforms");
    std::string cntTrim = trimWhitespace(cntRaw);
    if (!cntTrim.empty() && cntTrim != "0") {
      sendJsonError(500,
                    "上级平台列表主查询失败，表内仍有数据；请查看服务日志【execPsql】中的 PostgreSQL 报错（常见：缺列、"
                    "连接配置与 psql 不一致）");
      return;
    }
    const char emptyBody[] =
        "{\"code\":0,\"message\":\"ok\",\"data\":{"
        "\"items\":[],\"page\":1,\"pageSize\":0,\"total\":0}}";
    sendJson(std::string(emptyBody));
    return;
  }

  std::vector<std::string> lines = split(out, '\n');
  std::string items = "[";
  int count = 0;

  for (const auto& lineRaw : lines) {
    if (lineRaw.empty()) continue;
    std::vector<std::string> cols = split(lineRaw, '|');
    if (cols.size() < 14) continue;

    if (count > 0) {
      items += ",";
    }
    ++count;

    const std::string& id = cols[0];
    const std::string& name = cols[1];
    const std::string& sipDomain = cols[2];
    const std::string& gbId = cols[3];
    const std::string& sipIp = cols[4];
    const std::string& sipPort = cols[5];
    const std::string& transport = cols[6];
    const std::string& regUsername = cols[7];
    std::string regPasswordPlain = postgresHexFieldDecode(cols[8]);
    int registerExpires = cols.size() > 9 ? std::atoi(cols[9].c_str()) : 3600;
    if (registerExpires <= 0) registerExpires = 3600;
    bool enabled = (cols[10] == "t" || cols[10] == "true");
    int heartbeatInterval = cols.size() > 11 ? std::atoi(cols[11].c_str()) : 60;
    if (heartbeatInterval <= 0) heartbeatInterval = 60;
    bool online = cols.size() > 12 && (cols[12] == "t" || cols[12] == "true");
    std::string lastHb = cols[13];
    std::string scopeCsv;
    auto scIt = scopeByPlatform.find(id);
    if (scIt != scopeByPlatform.end()) scopeCsv = scIt->second;
    std::string exclCsv;
    auto exIt = excludeCamByPlatform.find(id);
    if (exIt != excludeCamByPlatform.end()) exclCsv = exIt->second;

    items += "{";
    items += "\"id\":" + (id.empty() ? std::string("0") : id) + ",";
    items += "\"name\":\"" + escapeJsonString(name) + "\",";
    items += "\"sipDomain\":\"" + escapeJsonString(sipDomain) + "\",";
    items += "\"gbId\":\"" + escapeJsonString(gbId) + "\",";
    items += "\"sipIp\":\"" + escapeJsonString(sipIp) + "\",";
    items += "\"sipPort\":" + (sipPort.empty() ? std::string("0") : sipPort) + ",";
    items += "\"transport\":\"" + escapeJsonString(transport) + "\",";
    items += "\"regUsername\":\"" + escapeJsonString(regUsername) + "\",";
    items += "\"regPassword\":\"" + escapeJsonString(regPasswordPlain) + "\",";
    items += "\"registerExpires\":" + std::to_string(registerExpires) + ",";
    items += "\"enabled\":" + jsonBool(enabled) + ",";
    items += "\"heartbeatInterval\":" + std::to_string(heartbeatInterval) + ",";
    items += "\"online\":" + jsonBool(online) + ",";
    items += "\"lastHeartbeatAt\":\"" + escapeJsonString(lastHb) + "\",";
    int scCount = 0;
    auto scCntIt = scopeCountByPlatform.find(id);
    if (scCntIt != scopeCountByPlatform.end()) scCount = scCntIt->second;
    int exCount = 0;
    auto exCntIt = excludeCountByPlatform.find(id);
    if (exCntIt != excludeCountByPlatform.end()) exCount = exCntIt->second;
    items += "\"scopeCount\":" + std::to_string(scCount) + ",";
    items += "\"excludedCount\":" + std::to_string(exCount);
    if (includeScopeDetails) {
      items += ",\"catalogGroupNodeIds\":[";
      {
        bool firstId = true;
        size_t pos = 0;
        while (pos < scopeCsv.size()) {
          size_t c = scopeCsv.find(',', pos);
          std::string tok = (c == std::string::npos) ? scopeCsv.substr(pos) : scopeCsv.substr(pos, c - pos);
          while (!tok.empty() && (tok[0] == ' ' || tok[0] == '\t')) tok.erase(tok.begin());
          while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t')) tok.pop_back();
          if (!tok.empty()) {
            if (!firstId) items += ",";
            firstId = false;
            items += tok;
          }
          if (c == std::string::npos) break;
          pos = c + 1;
        }
      }
      items += "]";
      items += ",\"excludedCameraIds\":[";
      {
        bool firstEx = true;
        size_t pos = 0;
        while (pos < exclCsv.size()) {
          size_t c = exclCsv.find(',', pos);
          std::string tok = (c == std::string::npos) ? exclCsv.substr(pos) : exclCsv.substr(pos, c - pos);
          while (!tok.empty() && (tok[0] == ' ' || tok[0] == '\t')) tok.erase(tok.begin());
          while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t')) tok.pop_back();
          if (!tok.empty()) {
            if (!firstEx) items += ",";
            firstEx = false;
            items += tok;
          }
          if (c == std::string::npos) break;
          pos = c + 1;
        }
      }
      items += "]";
    }
    items += "}";
  }
  items += "]";

  if (count == 0 && !trimWhitespace(out).empty()) {
    WarnL << "【上级平台列表】有输出但无有效数据行（每行需 14 列 | 分隔）。输出前 400 字符: "
          << trimWhitespace(out).substr(0, 400);
    sendJsonError(500,
                  "上级平台列表解析失败：列数与后端预期不符，或字段中含破坏分隔的换行；详情见服务日志");
    return;
  }

  std::string body = "{\"code\":0,\"message\":\"ok\",\"data\":{";
  body += "\"items\":" + items + ",";
  body += "\"page\":1,";
  body += "\"pageSize\":" + std::to_string(count) + ",";
  body += "\"total\":" + std::to_string(count);
  body += "}}";

  sendJson(body);
}

void HttpSession::sendPlatformCatalogScope(const std::string& idStr) {
  for (char ch : idStr) {
    if (ch < '0' || ch > '9') {
      sendJsonError(400, "无效的平台 ID");
      return;
    }
  }
  std::string sc = gb::execPsql(
      ("SELECT COALESCE(string_agg(catalog_group_node_id::text, ',' ORDER BY catalog_group_node_id), '') "
       "FROM upstream_catalog_scope WHERE upstream_platform_id=" +
       idStr)
          .c_str());
  std::string ex = gb::execPsql(
      ("SELECT COALESCE(string_agg(camera_id::text, ',' ORDER BY camera_id), '') "
       "FROM upstream_catalog_camera_exclude WHERE upstream_platform_id=" +
       idStr)
          .c_str());
  std::string scopeCsv = trimWhitespace(sc);
  std::string exclCsv = trimWhitespace(ex);
  if (scopeCsv == "ERROR") scopeCsv.clear();
  if (exclCsv == "ERROR") exclCsv.clear();
  std::string body = "{\"code\":0,\"message\":\"ok\",\"data\":{\"catalogGroupNodeIds\":[";
  bool first = true;
  size_t pos = 0;
  while (pos < scopeCsv.size()) {
    size_t c = scopeCsv.find(',', pos);
    std::string tok = (c == std::string::npos) ? scopeCsv.substr(pos) : scopeCsv.substr(pos, c - pos);
    tok = trimWhitespace(tok);
    if (!tok.empty()) {
      if (!first) body += ",";
      first = false;
      body += tok;
    }
    if (c == std::string::npos) break;
    pos = c + 1;
  }
  body += "],\"excludedCameraIds\":[";
  first = true;
  pos = 0;
  while (pos < exclCsv.size()) {
    size_t c = exclCsv.find(',', pos);
    std::string tok = (c == std::string::npos) ? exclCsv.substr(pos) : exclCsv.substr(pos, c - pos);
    tok = trimWhitespace(tok);
    if (!tok.empty()) {
      if (!first) body += ",";
      first = false;
      body += tok;
    }
    if (c == std::string::npos) break;
    pos = c + 1;
  }
  body += "]}}";
  sendJson(body);
}

void HttpSession::sendPlatformById(const std::string& idStr) {
  for (char ch : idStr) {
    if (ch < '0' || ch > '9') {
      sendJsonError(400, "无效的平台 ID");
      return;
    }
  }
  std::string out = gb::execPsql(
      ("SELECT u.id, u.name, u.sip_domain, u.gb_id, u.sip_ip, u.sip_port, u.transport, u.reg_username, "
       "encode(convert_to(COALESCE(u.reg_password,''), 'UTF8'), 'hex'), "
       "COALESCE(u.register_expires,3600), u.enabled, COALESCE(u.heartbeat_interval,60), u.online, "
       "COALESCE(to_char(u.last_heartbeat_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '') "
       "FROM upstream_platforms u WHERE u.id=" +
       idStr + " LIMIT 1")
          .c_str());
  std::string line = trimWhitespace(out);
  if (line.empty()) {
    sendJsonError(404, "未找到该上级平台");
    return;
  }
  size_t nl = line.find('\n');
  if (nl != std::string::npos) line = line.substr(0, nl);
  std::vector<std::string> cols = split(line, '|');
  if (cols.size() < 14) {
    sendJsonError(500, "平台数据解析失败");
    return;
  }
  std::string scopeRaw = gb::execPsql(
      ("SELECT COALESCE(string_agg(catalog_group_node_id::text, ',' ORDER BY catalog_group_node_id), '') "
       "FROM upstream_catalog_scope WHERE upstream_platform_id=" +
       idStr)
          .c_str());
  std::string exclRaw = gb::execPsql(
      ("SELECT COALESCE(string_agg(camera_id::text, ',' ORDER BY camera_id), '') "
       "FROM upstream_catalog_camera_exclude WHERE upstream_platform_id=" +
       idStr)
          .c_str());
  std::string scopeCsv = trimWhitespace(scopeRaw);
  std::string exclCsv = trimWhitespace(exclRaw);
  std::string body = "{\"code\":0,\"message\":\"ok\",\"data\":{";
  body += "\"id\":" + cols[0] + ",";
  body += "\"name\":\"" + escapeJsonString(cols[1]) + "\",";
  body += "\"sipDomain\":\"" + escapeJsonString(cols[2]) + "\",";
  body += "\"gbId\":\"" + escapeJsonString(cols[3]) + "\",";
  body += "\"sipIp\":\"" + escapeJsonString(cols[4]) + "\",";
  body += "\"sipPort\":" + (cols[5].empty() ? std::string("0") : cols[5]) + ",";
  body += "\"transport\":\"" + escapeJsonString(cols[6]) + "\",";
  body += "\"regUsername\":\"" + escapeJsonString(cols[7]) + "\",";
  body += "\"regPassword\":\"" + escapeJsonString(postgresHexFieldDecode(cols[8])) + "\",";
  body += "\"registerExpires\":" + std::to_string(std::max(1, std::atoi(cols[9].c_str()))) + ",";
  body += "\"enabled\":" + jsonBool(cols[10] == "t" || cols[10] == "true") + ",";
  body += "\"heartbeatInterval\":" + std::to_string(std::max(1, std::atoi(cols[11].c_str()))) + ",";
  body += "\"online\":" + jsonBool(cols[12] == "t" || cols[12] == "true") + ",";
  body += "\"lastHeartbeatAt\":\"" + escapeJsonString(cols[13]) + "\",";
  body += "\"catalogGroupNodeIds\":[";
  bool first = true;
  size_t pos = 0;
  while (pos < scopeCsv.size()) {
    size_t c = scopeCsv.find(',', pos);
    std::string tok = (c == std::string::npos) ? scopeCsv.substr(pos) : scopeCsv.substr(pos, c - pos);
    tok = trimWhitespace(tok);
    if (!tok.empty()) {
      if (!first) body += ",";
      first = false;
      body += tok;
    }
    if (c == std::string::npos) break;
    pos = c + 1;
  }
  body += "],\"excludedCameraIds\":[";
  first = true;
  pos = 0;
  while (pos < exclCsv.size()) {
    size_t c = exclCsv.find(',', pos);
    std::string tok = (c == std::string::npos) ? exclCsv.substr(pos) : exclCsv.substr(pos, c - pos);
    tok = trimWhitespace(tok);
    if (!tok.empty()) {
      if (!first) body += ",";
      first = false;
      body += tok;
    }
    if (c == std::string::npos) break;
    pos = c + 1;
  }
  body += "]}}";
  sendJson(body);
}

void HttpSession::sendDevicePlatforms(const std::string& query) {
  int page = 1;
  int pageSize = 10;
  std::string ps = getQueryParam(query, "page");
  if (!ps.empty()) {
    int v = std::atoi(ps.c_str());
    if (v >= 1) page = v;
  }
  ps = getQueryParam(query, "pageSize");
  if (!ps.empty()) {
    int v = std::atoi(ps.c_str());
    if (v >= 1 && v <= 100) pageSize = v;
  }
  std::string keyword = getQueryParam(query, "keyword");
  std::string whitelist = getQueryParam(query, "whitelist");
  std::string blacklist = getQueryParam(query, "blacklist");
  std::string mode = getQueryParam(query, "mode");
  std::string onlineParam = getQueryParam(query, "online");

  std::string where = "1=1";
  if (!keyword.empty()) {
    std::string k = gb::escapeSqlString(keyword);
    where += " AND (name ILIKE '%" + k + "%' OR gb_id ILIKE '%" + k + "%')";
  }
  if (whitelist == "true" || whitelist == "1") where += " AND list_type='whitelist'";
  if (blacklist == "true" || blacklist == "1") where += " AND list_type='blacklist'";
  if (!mode.empty() && (mode == "inherit" || mode == "custom")) where += " AND strategy_mode='" + gb::escapeSqlString(mode) + "'";
  if (onlineParam == "true" || onlineParam == "1") where += " AND online = true";
  else if (onlineParam == "false" || onlineParam == "0") where += " AND online = false";

  std::string countSql = "SELECT count(*) FROM device_platforms WHERE " + where;
  std::string countOut = gb::execPsql(countSql.c_str());
  int total = 0;
  if (!countOut.empty()) {
    size_t nl = countOut.find('\n');
    std::string line = (nl == std::string::npos) ? countOut : countOut.substr(0, nl);
    total = std::atoi(line.c_str());
  }
  int offset = (page - 1) * pageSize;
  // 使用子查询实时统计摄像头数量
  std::string sql = "SELECT dp.id, dp.name, dp.gb_id, "
      "COALESCE((SELECT COUNT(*) FROM cameras c WHERE c.platform_id = dp.id), 0), "
      "dp.list_type, dp.strategy_mode, dp.online, COALESCE(dp.custom_auth_password,''), "
      "COALESCE(dp.custom_media_host,''), dp.custom_media_port, COALESCE(dp.stream_media_url,''), "
      "COALESCE(dp.stream_rtp_transport::text, ''), "
      "COALESCE(dp.contact_ip,''), COALESCE(dp.contact_port,0), "
      "COALESCE(dp.signal_src_ip,''), COALESCE(dp.signal_src_port,0), "
      "COALESCE(to_char(dp.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD HH24:MI:SS'), ''), "
      "COALESCE(to_char(dp.last_heartbeat_at AT TIME ZONE 'UTC', 'YYYY-MM-DD HH24:MI:SS'), '') "
      "FROM device_platforms dp WHERE " + where + " ORDER BY dp.id LIMIT " + std::to_string(pageSize) + " OFFSET " + std::to_string(offset);
  std::string out = gb::execPsql(sql.c_str());

  std::string items = "[";
  int count = 0;
  if (!out.empty()) {
    std::vector<std::string> lines = split(out, '\n');
    for (const auto& lineRaw : lines) {
      if (lineRaw.empty()) continue;
      std::vector<std::string> cols = split(lineRaw, '|');
      if (cols.size() < 7) continue;
      if (count > 0) items += ",";
      ++count;
      std::string id = cols[0];
      std::string name = cols[1];
      std::string gbId = cols[2];
      std::string cameraCount = cols.size() > 3 ? cols[3] : "0";
      std::string listType = cols.size() > 4 ? cols[4] : "normal";
      std::string strategyMode = cols.size() > 5 ? cols[5] : "inherit";
      std::string o = cols.size() > 6 ? cols[6] : "";
      std::string customAuth = cols.size() > 7 ? cols[7] : "";
      std::string customHost = cols.size() > 8 ? cols[8] : "";
      std::string customPort = cols.size() > 9 ? cols[9] : "";
      std::string streamUrl = cols.size() > 10 ? cols[10] : "";
      std::string streamRtpRaw = cols.size() > 11 ? cols[11] : "";
      std::string contactIp = cols.size() > 12 ? cols[12] : "";
      std::string contactPortStr = cols.size() > 13 ? cols[13] : "0";
      std::string signalSrcIp = cols.size() > 14 ? cols[14] : "";
      std::string signalSrcPortStr = cols.size() > 15 ? cols[15] : "0";
      std::string createdAt = cols.size() > 16 ? cols[16] : "";
      std::string lastHeartbeatAt = cols.size() > 17 ? cols[17] : "";
      trimR(listType); trimR(strategyMode); trimR(o); trimR(customAuth); trimR(customHost); trimR(customPort); trimR(streamUrl);
      trimR(streamRtpRaw); trimR(contactIp); trimR(signalSrcIp); trimR(createdAt); trimR(lastHeartbeatAt);
      bool online = (o == "t" || o == "true");
      int port = 0;
      if (!customPort.empty() && customPort != " ") port = std::atoi(customPort.c_str());
      int contactPort = 0;
      if (!contactPortStr.empty() && contactPortStr != " ") contactPort = std::atoi(contactPortStr.c_str());
      int signalSrcPort = 0;
      if (!signalSrcPortStr.empty() && signalSrcPortStr != " ") signalSrcPort = std::atoi(signalSrcPortStr.c_str());
      std::string addr = customHost;
      if (port > 0) addr = addr + ":" + customPort;
      std::string streamRtpJson;
      if (streamRtpRaw == "tcp" || streamRtpRaw == "udp")
        streamRtpJson = "\"streamRtpTransport\":\"" + streamRtpRaw + "\"";
      else
        streamRtpJson = "\"streamRtpTransport\":null";
      items += "{\"id\":" + (id.empty() ? "0" : id) + ",\"name\":\"" + name + "\",\"gbId\":\"" + gbId + "\",\"cameraCount\":" + cameraCount + ",\"listType\":\"" + listType + "\",\"strategyMode\":\"" + strategyMode + "\",\"online\":" + jsonBool(online) + ",\"customAuthPassword\":\"" + escapeJsonString(customAuth) + "\",\"customMediaHost\":\"" + escapeJsonString(customHost) + "\",\"customMediaPort\":" + std::to_string(port) + ",\"streamMediaUrl\":\"" + escapeJsonString(streamUrl) + "\"," + streamRtpJson + ",\"contactIp\":\"" + escapeJsonString(contactIp) + "\",\"contactPort\":" + std::to_string(contactPort) + ",\"signalSrcIp\":\"" + escapeJsonString(signalSrcIp) + "\",\"signalSrcPort\":" + std::to_string(signalSrcPort) + ",\"createdAt\":\"" + escapeJsonString(createdAt) + "\",\"lastHeartbeatAt\":\"" + escapeJsonString(lastHeartbeatAt) + "\"}";
    }
  }
  items += "]";
  std::string body = "{\"code\":0,\"message\":\"ok\",\"data\":{\"items\":" + items + ",\"page\":" + std::to_string(page) + ",\"pageSize\":" + std::to_string(pageSize) + ",\"total\":" + std::to_string(total) + "}}";
  sendJson(body);
}

void HttpSession::handlePostDevicePlatform(const std::string& body) {
  std::string name = extractJsonString(body, "name");
  std::string gbId = extractJsonString(body, "gbId");
  if (name.empty() || gbId.empty()) {
    sendJsonError(400, "缺少必填字段 name 或 gbId");
    return;
  }
  std::string listType = extractJsonString(body, "listType");
  if (listType.empty()) listType = "normal";
  if (listType != "normal" && listType != "whitelist" && listType != "blacklist") listType = "normal";
  std::string strategyMode = extractJsonString(body, "strategyMode");
  if (strategyMode.empty()) strategyMode = "inherit";
  if (strategyMode == "independent") strategyMode = "custom";
  if (strategyMode != "inherit" && strategyMode != "custom") strategyMode = "inherit";
  std::string customAuth = extractJsonString(body, "customAuthPassword");
  std::string customHost = extractJsonString(body, "customMediaHost");
  int customPort = extractJsonInt(body, "customMediaPort");
  std::string streamUrl = extractJsonString(body, "streamMediaUrl");
  std::string streamRtp = extractJsonString(body, "streamRtpTransport");
  bool streamRtpExplicitNull = jsonKeyHasNull(body, "streamRtpTransport");
  std::string rtpFrag = "NULL";
  if (strategyMode == "custom") {
    if (!streamRtpExplicitNull && streamRtp == "tcp")
      rtpFrag = "'tcp'";
    else if (!streamRtpExplicitNull && streamRtp == "udp")
      rtpFrag = "'udp'";
    else
      rtpFrag = "NULL";
  }

  std::string sql = "INSERT INTO device_platforms (name, gb_id, list_type, strategy_mode, custom_auth_password, custom_media_host, custom_media_port, stream_media_url, stream_rtp_transport) VALUES ('" +
      gb::escapeSqlString(name) + "','" + gb::escapeSqlString(gbId) + "','" + gb::escapeSqlString(listType) + "','" +
      gb::escapeSqlString(strategyMode) + "','" + gb::escapeSqlString(customAuth) + "','" +
      gb::escapeSqlString(customHost) + "'," + (customPort > 0 ? std::to_string(customPort) : "NULL") + ",'" +
      gb::escapeSqlString(streamUrl) + "'," + rtpFrag + ")";
  if (!gb::execPsqlCommand(sql)) {
    sendJsonError(500, "新增失败");
    return;
  }
  sendJson("{\"code\":0,\"message\":\"ok\",\"data\":null}");
}

void HttpSession::handlePutDevicePlatform(const std::string& idStr, const std::string& body) {
  for (size_t i = 0; i < idStr.size(); ++i) {
    if (idStr[i] < '0' || idStr[i] > '9') {
      sendJsonError(400, "无效的平台 ID");
      return;
    }
  }
  std::vector<std::string> sets;
  std::string name = extractJsonString(body, "name");
  if (!name.empty()) sets.push_back("name='" + gb::escapeSqlString(name) + "'");
  std::string gbId = extractJsonString(body, "gbId");
  if (!gbId.empty()) sets.push_back("gb_id='" + gb::escapeSqlString(gbId) + "'");
  std::string listType = extractJsonString(body, "listType");
  if (!listType.empty() && (listType == "normal" || listType == "whitelist" || listType == "blacklist"))
    sets.push_back("list_type='" + gb::escapeSqlString(listType) + "'");
  std::string strategyMode = extractJsonString(body, "strategyMode");
  if (!strategyMode.empty()) {
    if (strategyMode == "custom" || strategyMode == "independent") strategyMode = "custom";
    else strategyMode = "inherit";
    sets.push_back("strategy_mode='" + gb::escapeSqlString(strategyMode) + "'");
  }
  std::string customAuth = extractJsonString(body, "customAuthPassword");
  sets.push_back("custom_auth_password='" + gb::escapeSqlString(customAuth) + "'");
  std::string customHost = extractJsonString(body, "customMediaHost");
  if (!customHost.empty()) sets.push_back("custom_media_host='" + gb::escapeSqlString(customHost) + "'");
  else sets.push_back("custom_media_host=NULL");
  int customPort = extractJsonInt(body, "customMediaPort");
  if (customPort > 0) sets.push_back("custom_media_port=" + std::to_string(customPort));
  else sets.push_back("custom_media_port=NULL");
  std::string streamUrl = extractJsonString(body, "streamMediaUrl");
  sets.push_back("stream_media_url='" + gb::escapeSqlString(streamUrl) + "'");

  std::string strategyInBody = extractJsonString(body, "strategyMode");
  if (!strategyInBody.empty()) {
    std::string norm = strategyInBody;
    if (norm == "independent") norm = "custom";
    if (norm == "inherit") {
      sets.push_back("stream_rtp_transport=NULL");
    } else if (norm == "custom") {
      std::string streamRtp = extractJsonString(body, "streamRtpTransport");
      bool streamRtpExplicitNull = jsonKeyHasNull(body, "streamRtpTransport");
      if (streamRtpExplicitNull || streamRtp.empty())
        sets.push_back("stream_rtp_transport=NULL");
      else if (streamRtp == "tcp")
        sets.push_back("stream_rtp_transport='tcp'");
      else
        sets.push_back("stream_rtp_transport='udp'");
    }
  } else if (body.find("\"streamRtpTransport\"") != std::string::npos) {
    std::string streamRtp = extractJsonString(body, "streamRtpTransport");
    bool streamRtpExplicitNull = jsonKeyHasNull(body, "streamRtpTransport");
    if (streamRtpExplicitNull || streamRtp.empty())
      sets.push_back("stream_rtp_transport=NULL");
    else if (streamRtp == "tcp")
      sets.push_back("stream_rtp_transport='tcp'");
    else
      sets.push_back("stream_rtp_transport='udp'");
  }

  sets.push_back("updated_at=CURRENT_TIMESTAMP");

  std::string sql = "UPDATE device_platforms SET ";
  for (size_t i = 0; i < sets.size(); ++i) {
    if (i > 0) sql += ",";
    sql += sets[i];
  }
  sql += " WHERE id=" + idStr;
  if (!gb::execPsqlCommand(sql)) {
    sendJsonError(500, "更新失败");
    return;
  }
  sendJson("{\"code\":0,\"message\":\"ok\",\"data\":null}");
}

void HttpSession::sendAlarms(const std::string& query) {
  int page = 1;
  int pageSize = 10;
  std::string ps = getQueryParam(query, "page");
  if (!ps.empty()) {
    int v = std::atoi(ps.c_str());
    if (v >= 1) page = v;
  }
  ps = getQueryParam(query, "pageSize");
  if (!ps.empty()) {
    int v = std::atoi(ps.c_str());
    if (v >= 1 && v <= 100) pageSize = v;
  }
  std::string levelParam = getQueryParam(query, "level");
  std::string statusParam = getQueryParam(query, "status");

  std::string where = "1=1";
  if (!levelParam.empty() && (levelParam == "info" || levelParam == "major" || levelParam == "critical"))
    where += " AND level='" + gb::escapeSqlString(levelParam) + "'";
  if (!statusParam.empty() && (statusParam == "new" || statusParam == "ack" || statusParam == "disposed"))
    where += " AND status='" + gb::escapeSqlString(statusParam) + "'";

  std::string countSql = "SELECT count(*) FROM alarms WHERE " + where;
  std::string countOut = gb::execPsql(countSql.c_str());
  int total = 0;
  if (!countOut.empty()) {
    size_t nl = countOut.find('\n');
    std::string line = (nl == std::string::npos) ? countOut : countOut.substr(0, nl);
    total = std::atoi(line.c_str());
  }
  int offset = (page - 1) * pageSize;
  std::string sql = "SELECT id, COALESCE(channel_id,''), COALESCE(channel_name,''), level, status, COALESCE(description,''), occurred_at FROM alarms WHERE " + where + " ORDER BY occurred_at DESC NULLS LAST, id DESC LIMIT " + std::to_string(pageSize) + " OFFSET " + std::to_string(offset);
  std::string out = gb::execPsql(sql.c_str());

  std::string items = "[";
  int count = 0;
  if (!out.empty()) {
    std::vector<std::string> lines = split(out, '\n');
    for (const auto& lineRaw : lines) {
      if (lineRaw.empty()) continue;
      std::vector<std::string> cols = split(lineRaw, '|');
      if (cols.size() < 6) continue;
      if (count > 0) items += ",";
      ++count;
      std::string id = cols[0];
      std::string channelId = cols.size() > 1 ? cols[1] : "";
      std::string channelName = cols.size() > 2 ? cols[2] : "";
      std::string level = cols.size() > 3 ? cols[3] : "info";
      std::string status = cols.size() > 4 ? cols[4] : "new";
      std::string desc = cols.size() > 5 ? cols[5] : "";
      std::string occurredAt = cols.size() > 6 ? cols[6] : "";
      trimR(channelId); trimR(channelName); trimR(level); trimR(status); trimR(desc); trimR(occurredAt);
      items += "{\"id\":" + (id.empty() ? "0" : id) + ",\"channelId\":\"" + escapeJsonString(channelId) + "\",\"channelName\":\"" + escapeJsonString(channelName) + "\",\"level\":\"" + level + "\",\"status\":\"" + status + "\",\"description\":\"" + escapeJsonString(desc) + "\",\"occurredAt\":\"" + escapeJsonString(occurredAt) + "\"}";
    }
  }
  items += "]";
  std::string body = "{\"code\":0,\"message\":\"ok\",\"data\":{\"items\":" + items + ",\"page\":" + std::to_string(page) + ",\"pageSize\":" + std::to_string(pageSize) + ",\"total\":" + std::to_string(total) + "}}";
  sendJson(body);
}

void HttpSession::handlePostAlarm(const std::string& body) {
  std::string channelId = extractJsonString(body, "channelId");
  std::string channelName = extractJsonString(body, "channelName");
  std::string level = extractJsonString(body, "level");
  std::string description = extractJsonString(body, "description");
  if (level.empty()) level = "info";
  if (level != "info" && level != "major" && level != "critical") level = "info";
  std::string sql = "INSERT INTO alarms (channel_id, channel_name, level, status, description) VALUES ('" +
      gb::escapeSqlString(channelId) + "','" + gb::escapeSqlString(channelName) + "','" +
      gb::escapeSqlString(level) + "','new','" + gb::escapeSqlString(description) + "')";
  if (!gb::execPsqlCommand(sql)) {
    sendJsonError(500, "上报告警失败");
    return;
  }
  sendJson("{\"code\":0,\"message\":\"ok\",\"data\":null}");
}

void HttpSession::handlePutAlarm(const std::string& idStr, const std::string& body) {
  for (size_t i = 0; i < idStr.size(); ++i) {
    if (idStr[i] < '0' || idStr[i] > '9') {
      sendJsonError(400, "无效的告警 ID");
      return;
    }
  }
  std::string status = extractJsonString(body, "status");
  if (status != "ack" && status != "disposed") {
    sendJsonError(400, "status 需为 ack 或 disposed");
    return;
  }
  std::string disposeNote = extractJsonString(body, "disposeNote");
  std::string sql = "UPDATE alarms SET status='" + gb::escapeSqlString(status) + "',";
  if (status == "ack") sql += "ack_at=CURRENT_TIMESTAMP,";
  sql += "dispose_note='" + gb::escapeSqlString(disposeNote) + "',updated_at=CURRENT_TIMESTAMP WHERE id=" + idStr;
  if (!gb::execPsqlCommand(sql)) {
    sendJsonError(500, "更新失败");
    return;
  }
  sendJson("{\"code\":0,\"message\":\"ok\",\"data\":null}");
}

static int queryCount(const char* sql) {
  std::string out = gb::execPsql(sql);
  if (out.empty()) return 0;
  size_t nl = out.find('\n');
  std::string line = (nl == std::string::npos) ? out : out.substr(0, nl);
  return std::atoi(line.c_str());
}

void HttpSession::sendOverview() {
  int deviceTotal = queryCount("SELECT count(*) FROM device_platforms");
  int deviceOnline = queryCount("SELECT count(*) FROM device_platforms WHERE online = true");
  int cameraTotal = queryCount("SELECT count(*) FROM cameras");
  int cameraOnline = queryCount("SELECT count(*) FROM cameras WHERE online = true");
  int alarmNew = queryCount("SELECT count(*) FROM alarms WHERE status = 'new'");
  int upstreamTotal = queryCount("SELECT count(*) FROM upstream_platforms");
  std::string body = "{\"code\":0,\"message\":\"ok\",\"data\":{";
  body += "\"devicePlatformTotal\":" + std::to_string(deviceTotal) + ",";
  body += "\"devicePlatformOnline\":" + std::to_string(deviceOnline) + ",";
  body += "\"cameraTotal\":" + std::to_string(cameraTotal) + ",";
  body += "\"cameraOnline\":" + std::to_string(cameraOnline) + ",";
  body += "\"alarmNewCount\":" + std::to_string(alarmNew) + ",";
  body += "\"upstreamPlatformTotal\":" + std::to_string(upstreamTotal);
  body += "}}";
  sendJson(body);
}

void HttpSession::sendReplaySegments(const std::string& query) {
  std::string cameraId = getQueryParam(query, "cameraId");
  std::string startTime = getQueryParam(query, "startTime");
  std::string endTime = getQueryParam(query, "endTime");
  std::string platformGbId = getQueryParam(query, "platformGbId");
  if (platformGbId.empty()) {
    std::string pid = getQueryParam(query, "platformId");
    if (pid.empty()) pid = getQueryParam(query, "platformDbId");
    if (!pid.empty()) {
      if (looksLikeGbId(pid))
        platformGbId = pid;
      else if (isNumericId(pid))
        platformGbId = queryPlatformGbIdByPlatformId(pid);
    }
  }
  if (cameraId.empty() || startTime.empty() || endTime.empty()) {
    sendJsonError(400, "缺少 cameraId、startTime 或 endTime");
    return;
  }
  std::string searchErr;
  if (!gb::runRecordInfoSearchBlocking(cameraId, platformGbId, startTime, endTime, searchErr)) {
    sendJsonError(503, searchErr.empty() ? "录像检索失败或超时" : searchErr);
    return;
  }
  std::string dbId, devGb, platGbRow;
  if (!gb::resolveCameraRowByPathSegment(cameraId, platformGbId, dbId, devGb, platGbRow)) {
    sendJsonError(404, "摄像头不存在");
    return;
  }
  // 展示语义：开始时间 ≤ 结束时间；时长按库中起止时刻真实跨度（兼容历史错序行）
  std::string sql =
      "SELECT s.id, REPLACE(COALESCE(s.segment_id,''), '|', '-'), "
      "to_char(LEAST(s.start_time, s.end_time) AT TIME ZONE 'Asia/Shanghai', 'YYYY-MM-DD HH24:MI:SS'), "
      "to_char(GREATEST(s.start_time, s.end_time) AT TIME ZONE 'Asia/Shanghai', 'YYYY-MM-DD HH24:MI:SS'), "
      "GREATEST(0, FLOOR(EXTRACT(EPOCH FROM (GREATEST(s.start_time, s.end_time) - "
      "LEAST(s.start_time, s.end_time))))::integer), "
      "CASE WHEN s.downloadable THEN 't' ELSE 'f' END, "
      "FLOOR(EXTRACT(EPOCH FROM LEAST(s.start_time, s.end_time)))::bigint, "
      "FLOOR(EXTRACT(EPOCH FROM GREATEST(s.start_time, s.end_time)))::bigint "
      "FROM replay_segments s JOIN replay_tasks t ON t.id = s.task_id WHERE t.camera_id=" + dbId +
      " AND s.start_time < '" + gb::escapeSqlString(endTime) +
      "'::timestamptz AND s.end_time > '" + gb::escapeSqlString(startTime) +
      "'::timestamptz ORDER BY LEAST(s.start_time, s.end_time)";
  std::string out = gb::execPsql(sql.c_str());

  std::string items = "[";
  int count = 0;
  if (!out.empty()) {
    std::vector<std::string> lines = split(out, '\n');
    for (const auto& lineRaw : lines) {
      if (lineRaw.empty()) continue;
      std::vector<std::string> cols = split(lineRaw, '|');
      if (cols.size() < 4) continue;
      if (count > 0) items += ",";
      ++count;
      std::string id = cols[0];
      std::string segmentId = cols.size() > 1 ? cols[1] : "";
      std::string segStart = cols.size() > 2 ? cols[2] : "";
      std::string segEnd = cols.size() > 3 ? cols[3] : "";
      int durationSec = cols.size() > 4 ? std::atoi(cols[4].c_str()) : 0;
      std::string downloadable = (cols.size() > 5 && (cols[5].find('f') != std::string::npos)) ? "false" : "true";
      std::string startUnix = cols.size() > 6 ? cols[6] : "0";
      std::string endUnix = cols.size() > 7 ? cols[7] : "0";
      trimR(segmentId); trimR(segStart); trimR(segEnd);
      trimR(startUnix);
      trimR(endUnix);
      items += "{\"id\":" + (id.empty() ? "0" : id) + ",\"segmentId\":\"" + escapeJsonString(segmentId) +
               "\",\"startTime\":\"" + escapeJsonString(segStart) + "\",\"endTime\":\"" + escapeJsonString(segEnd) +
               "\",\"durationSeconds\":" + std::to_string(durationSec) + ",\"downloadable\":" + downloadable +
               ",\"startTimeUnix\":" + (startUnix.empty() ? "0" : startUnix) +
               ",\"endTimeUnix\":" + (endUnix.empty() ? "0" : endUnix) + "}";
    }
  }
  items += "]";
  std::string body = "{\"code\":0,\"message\":\"ok\",\"data\":{\"items\":" + items + "}}";
  sendJson(body);
}

void HttpSession::handleDeleteDevicePlatform(const std::string& idStr) {
  for (size_t i = 0; i < idStr.size(); ++i) {
    if (idStr[i] < '0' || idStr[i] > '9') {
      sendJsonError(400, "无效的平台 ID");
      return;
    }
  }
  std::string sql = "DELETE FROM device_platforms WHERE id=" + idStr;
  if (!gb::execPsqlCommand(sql)) {
    sendJsonError(500, "删除失败");
    return;
  }
  sendJson("{\"code\":0,\"message\":\"ok\",\"data\":null}");
}

void HttpSession::sendCameras(const std::string& query) {
  int page = 1;
  int pageSize = 10;
  std::string ps = getQueryParam(query, "page");
  if (!ps.empty()) {
    int v = std::atoi(ps.c_str());
    if (v >= 1) page = v;
  }
  ps = getQueryParam(query, "pageSize");
  if (!ps.empty()) {
    int v = std::atoi(ps.c_str());
    if (v >= 1 && v <= 100) pageSize = v;
  }
  std::string keyword = getQueryParam(query, "keyword");
  std::string platformKeyword = getQueryParam(query, "platformKeyword");
  std::string onlineParam = getQueryParam(query, "online");
  std::string listTypeParam = getQueryParam(query, "listType");
  std::string strategyModeParam = getQueryParam(query, "strategyMode");

  std::string where = "1=1";
  if (!keyword.empty()) {
    std::string k = gb::escapeSqlString(keyword);
    where += " AND (c.name ILIKE '%" + k + "%' OR CAST(c.id AS TEXT) ILIKE '%" + k + "%' OR c.device_gb_id ILIKE '%" +
             k + "%')";
  }
  if (!platformKeyword.empty()) {
    std::string pk = gb::escapeSqlString(platformKeyword);
    where += " AND (p.name ILIKE '%" + pk + "%' OR p.gb_id ILIKE '%" + pk + "%')";
  }
  if (onlineParam == "true" || onlineParam == "1") where += " AND c.online = true";
  else if (onlineParam == "false" || onlineParam == "0") where += " AND c.online = false";
  // 与 device_platforms.list_type / strategy_mode 一致（摄像头经 platform_id 关联平台）
  if (listTypeParam == "normal" || listTypeParam == "whitelist" || listTypeParam == "blacklist") {
    where += " AND COALESCE(NULLIF(TRIM(p.list_type),''),'normal')='" + gb::escapeSqlString(listTypeParam) + "'";
  }
  if (strategyModeParam == "inherit" || strategyModeParam == "custom") {
    where += " AND COALESCE(NULLIF(TRIM(p.strategy_mode),''),'inherit')='" + gb::escapeSqlString(strategyModeParam) +
             "'";
  }

  std::string countSql = "SELECT count(*) FROM cameras c LEFT JOIN device_platforms p ON c.platform_id = p.id WHERE " + where;
  std::string countOut = gb::execPsql(countSql.c_str());
  int total = 0;
  if (!countOut.empty()) {
    size_t nl = countOut.find('\n');
    std::string line = (nl == std::string::npos) ? countOut : countOut.substr(0, nl);
    total = std::atoi(line.c_str());
  }
  int offset = (page - 1) * pageSize;
  // 返回平台国标ID（platformGbId）供前端展示，同时返回平台数字ID供前端传回
  std::string sql = "SELECT c.id::text, c.device_gb_id, c.name, COALESCE(p.gb_id,''), COALESCE(p.name,''), c.online, "
      "COALESCE(c.manufacturer,''), COALESCE(c.model,''), COALESCE(c.civil_code,''), COALESCE(c.address,''), c.platform_id::text, "
      "COALESCE(NULLIF(TRIM(p.list_type),''),'normal'), COALESCE(NULLIF(TRIM(p.strategy_mode),''),'inherit') "
      "FROM cameras c LEFT JOIN device_platforms p ON c.platform_id = p.id WHERE " + where + " ORDER BY c.id LIMIT " +
      std::to_string(pageSize) + " OFFSET " + std::to_string(offset);
  std::string out = gb::execPsql(sql.c_str());

  std::string items = "[";
  int count = 0;
  if (!out.empty()) {
    std::vector<std::string> lines = split(out, '\n');
    for (const auto& lineRaw : lines) {
      if (lineRaw.empty()) continue;
      std::vector<std::string> cols = split(lineRaw, '|');
      if (cols.size() < 6) continue;
      if (count > 0) items += ",";
      ++count;
      std::string id = cols[0];
      std::string deviceGbId = cols[1];
      std::string name = cols[2];
      std::string platformGbId = cols.size() > 3 ? cols[3] : "";
      std::string platformName = cols.size() > 4 ? cols[4] : "";
      std::string o = cols.size() > 5 ? cols[5] : "";
      std::string manufacturer = cols.size() > 6 ? cols[6] : "";
      std::string model = cols.size() > 7 ? cols[7] : "";
      std::string civilCode = cols.size() > 8 ? cols[8] : "";
      std::string address = cols.size() > 9 ? cols[9] : "";
      std::string platformId = cols.size() > 10 ? cols[10] : "";
      std::string listTypeOut = cols.size() > 11 ? cols[11] : "normal";
      std::string strategyModeOut = cols.size() > 12 ? cols[12] : "inherit";
      trimR(deviceGbId);
      trimR(platformGbId); trimR(platformName); trimR(o); trimR(manufacturer); trimR(model); trimR(civilCode); trimR(address); trimR(platformId);
      trimR(listTypeOut);
      trimR(strategyModeOut);
      if (listTypeOut.empty()) listTypeOut = "normal";
      if (strategyModeOut.empty()) strategyModeOut = "inherit";
      bool online = (o == "t" || o == "true" || o == "1");
      std::string pidJson = (platformGbId.empty() || platformGbId == " ") ? "null" : "\"" + platformGbId + "\"";
      std::string platformIdJson = (platformId.empty() || platformId == " ") ? "null" : platformId;
      items += "{\"id\":\"" + id + "\",\"deviceGbId\":\"" + escapeJsonString(deviceGbId) + "\",\"name\":\"" +
               escapeJsonString(name) + "\",\"platformId\":" + pidJson + ",\"platformName\":\"" + platformName +
               "\",\"online\":" + jsonBool(online) + ",\"platformDbId\":" + platformIdJson +
               ",\"manufacturer\":\"" + escapeJsonString(manufacturer) + "\",\"model\":\"" + escapeJsonString(model) +
               "\",\"civilCode\":\"" + escapeJsonString(civilCode) + "\",\"address\":\"" + escapeJsonString(address) +
               "\",\"listType\":\"" + escapeJsonString(listTypeOut) + "\",\"strategyMode\":\"" +
               escapeJsonString(strategyModeOut) + "\"}";
    }
  }
  items += "]";
  std::string body = "{\"code\":0,\"message\":\"ok\",\"data\":{\"items\":" + items + ",\"page\":" + std::to_string(page) + ",\"pageSize\":" + std::to_string(pageSize) + ",\"total\":" + std::to_string(total) + "}}";
  sendJson(body);
}

void HttpSession::handleClearCameraRelatedData(const std::string& cameraId) {
  if (cameraId.empty() || cameraId.size() > 64 || cameraId.find('/') != std::string::npos) {
    sendJsonError(400, "无效的摄像头 ID");
    return;
  }
  std::string nodeRef;
  std::string platId;
  std::string deviceGb;
  std::string dbId;
  if (!fetchCameraCatalogKeys(cameraId, "", nodeRef, platId, deviceGb, dbId)) {
    sendJsonError(404, "摄像头不存在");
    return;
  }
  std::string escDev = gb::escapeSqlString(deviceGb);
  MediaService& mediaSvc = GetMediaService();
  mediaSvc.initialize();
  mediaSvc.closeSessionsForCamera(deviceGb);
  if (!gb::execPsqlCommand("DELETE FROM replay_tasks WHERE camera_id=" + dbId)) {
    sendJsonError(500, "清空录像检索数据失败");
    return;
  }
  gb::execPsqlCommand("DELETE FROM stream_sessions WHERE camera_db_id=" + dbId + " OR camera_id='" + escDev + "'");
  gb::execPsqlCommand("DELETE FROM alarms WHERE channel_id='" + escDev + "'");
  InfoL << "【handleClearCameraRelatedData】已清空摄像头关联库表（replay_tasks/级联片段与下载/stream_sessions/alarms） camera="
        << cameraId;
  sendJson("{\"code\":0,\"message\":\"ok\",\"data\":null}");
}

void HttpSession::handleDeleteCamera(const std::string& cameraId) {
  if (cameraId.empty() || cameraId.size() > 64 || cameraId.find('/') != std::string::npos) {
    sendJsonError(400, "无效的摄像头 ID");
    return;
  }
  std::string nodeRef;
  std::string platId;
  std::string deviceGb;
  std::string dbId;
  if (!fetchCameraCatalogKeys(cameraId, "", nodeRef, platId, deviceGb, dbId)) {
    sendJsonError(404, "摄像头不存在");
    return;
  }
  std::string escDev = gb::escapeSqlString(deviceGb);
  GetMediaService().closeSessionsForCamera(deviceGb);
  // 先解除 cameras.node_ref → catalog_nodes，否则 DELETE catalog_nodes 会因外键失败
  gb::execPsqlCommand("UPDATE cameras SET node_ref=NULL WHERE id=" + dbId);
  deleteCatalogNodesForCamera(escDev, nodeRef, platId);
  std::string delSql = "DELETE FROM cameras WHERE id=" + dbId;
  if (!gb::execPsqlCommand(delSql)) {
    sendJsonError(500, "删除失败");
    return;
  }
  sendJson("{\"code\":0,\"message\":\"ok\",\"data\":null}");
}

void HttpSession::handleBatchDeleteCameras(const std::string& body) {
  std::vector<std::string> ids;
  extractJsonCameraIds(body, ids);
  if (ids.empty()) {
    sendJsonError(400, "cameraIds 不能为空");
    return;
  }
  if (ids.size() > 100) {
    sendJsonError(400, "单次最多删除 100 条");
    return;
  }
  int deleted = 0;
  int notFound = 0;
  for (const auto& rawId : ids) {
    if (rawId.empty()) {
      continue;
    }
    if (rawId.size() > 64 || rawId.find('/') != std::string::npos) {
      continue;
    }
    std::string nodeRef;
    std::string platId;
    std::string deviceGb;
    std::string dbId;
    if (!fetchCameraCatalogKeys(rawId, "", nodeRef, platId, deviceGb, dbId)) {
      notFound++;
      continue;
    }
    std::string escDev = gb::escapeSqlString(deviceGb);
    GetMediaService().closeSessionsForCamera(deviceGb);
    gb::execPsqlCommand("UPDATE cameras SET node_ref=NULL WHERE id=" + dbId);
    deleteCatalogNodesForCamera(escDev, nodeRef, platId);
    if (gb::execPsqlCommand("DELETE FROM cameras WHERE id=" + dbId)) {
      deleted++;
    }
  }
  std::string res = "{\"code\":0,\"message\":\"ok\",\"data\":{\"deleted\":" + std::to_string(deleted) +
                     ",\"notFound\":" + std::to_string(notFound) + "}}";
  sendJson(res);
}

static std::string buildCatalogChildren(const std::vector<std::vector<std::string>>& rows, const std::string& parentId) {
  std::string arr = "[";
  int n = 0;
  for (const auto& r : rows) {
    std::string pid = r.size() > 1 ? r[1] : "";
    if (pid != parentId) continue;
    if (n > 0) arr += ",";
    ++n;
    std::string id = r[0];
    std::string code = r.size() > 2 ? r[2] : "";
    std::string name = r.size() > 3 ? r[3] : "";
    std::string levelStr = r.size() > 4 ? r[4] : "0";
    trimR(levelStr);
    std::string ch = buildCatalogChildren(rows, id);
    arr += "{\"id\":\"" + id + "\",\"name\":\"" + name + "\",\"code\":\"" + code + "\",\"level\":" + (levelStr.empty() ? "0" : levelStr) + ",\"children\":" + ch + "}";
  }
  arr += "]";
  return arr;
}

void HttpSession::sendCatalogNodes(const std::string& query) {
  // 获取查询参数
  std::string platformId = getQueryParam(query, "platformId");
  std::string nodeType = getQueryParam(query, "type");
  
  // 构建 WHERE 子句
  std::string where = "1=1";
  if (!platformId.empty()) {
    where += " AND platform_id = " + gb::escapeSqlString(platformId);
  }
  if (!nodeType.empty()) {
    where += " AND node_type = " + gb::escapeSqlString(nodeType);
  }
  
  // 查询新的 catalog_nodes 表结构
  std::string sql = "SELECT node_id, parent_id, name, node_type, manufacturer, model, status, platform_gb_id "
      "FROM catalog_nodes WHERE " + where + " ORDER BY COALESCE(parent_id,''), node_id";
  std::string out = gb::execPsql(sql.c_str());
  
  std::string items = "[";
  int count = 0;
  if (!out.empty()) {
    std::vector<std::string> lines = split(out, '\n');
    for (const auto& lineRaw : lines) {
      if (lineRaw.empty()) continue;
      std::vector<std::string> cols = split(lineRaw, '|');
      if (cols.size() < 4) continue;
      if (count > 0) items += ",";
      ++count;
      
      std::string nodeId = cols[0];
      std::string parentId = cols[1];
      std::string name = cols[2];
      std::string typeStr = cols.size() > 3 ? cols[3] : "0";
      std::string manufacturer = cols.size() > 4 ? cols[4] : "";
      std::string model = cols.size() > 5 ? cols[5] : "";
      std::string status = cols.size() > 6 ? cols[6] : "";
      std::string platformGbId = cols.size() > 7 ? cols[7] : "";
      
      trimR(parentId); trimR(name); trimR(typeStr); 
      trimR(manufacturer); trimR(model); trimR(status); trimR(platformGbId);
      
      int type = std::atoi(typeStr.c_str());
      std::string typeName = (type == 0) ? "device" : (type == 1) ? "directory" : "region";
      
      items += "{\"nodeId\":\"" + nodeId + "\",\"parentId\":\"" + parentId + "\",\"name\":\"" + 
               escapeJsonString(name) + "\",\"type\":" + typeStr + ",\"typeName\":\"" + typeName + 
               "\",\"manufacturer\":\"" + escapeJsonString(manufacturer) + "\",\"model\":\"" + 
               escapeJsonString(model) + "\",\"status\":\"" + status + "\",\"platformGbId\":\"" + 
               platformGbId + "\"}";
    }
  }
  items += "]";
  
  std::string body = "{\"code\":0,\"message\":\"ok\",\"data\":{\"items\":" + items + ",\"total\":" + std::to_string(count) + "}}";
  sendJson(body);
}

void HttpSession::sendCatalogNodeCameras(const std::string& nodeId) {
  if (nodeId.empty()) {
    sendJson("{\"code\":0,\"message\":\"ok\",\"data\":{\"items\":[]}}");
    return;
  }
  
  // 安全处理 nodeId
  std::string safeNodeId = gb::escapeSqlString(nodeId);
  
  // 查询该目录节点下的所有设备（通过 parent_id 关联）
  std::string sql = "SELECT c.id::text, c.device_gb_id, COALESCE(p.gb_id,''), c.name, c.platform_id::text, COALESCE(p.name,''), c.online, "
      "COALESCE(c.manufacturer,''), COALESCE(c.model,''), COALESCE(c.address,'') "
      "FROM cameras c "
      "LEFT JOIN device_platforms p ON c.platform_id = p.id "
      "WHERE c.node_id IN (SELECT node_id FROM catalog_nodes WHERE parent_id = '" + safeNodeId + "' AND node_type = 0) "
      "OR c.node_id = '" + safeNodeId + "' "
      "ORDER BY c.id";
  
  std::string out = gb::execPsql(sql.c_str());
  std::string items = "[";
  int count = 0;
  if (!out.empty()) {
    std::vector<std::string> lines = split(out, '\n');
    for (const auto& lineRaw : lines) {
      if (lineRaw.empty()) continue;
      std::vector<std::string> cols = split(lineRaw, '|');
      if (cols.size() < 7) continue;
      if (count > 0) items += ",";
      ++count;
      
      std::string id = cols[0];
      std::string deviceGbId = cols[1];
      std::string platformGbIdCol = cols.size() > 2 ? cols[2] : "";
      std::string name = cols.size() > 3 ? cols[3] : "";
      std::string platformId = cols.size() > 4 ? cols[4] : "0";
      std::string platformName = cols.size() > 5 ? cols[5] : "";
      std::string o = cols.size() > 6 ? cols[6] : "";
      std::string manufacturer = cols.size() > 7 ? cols[7] : "";
      std::string model = cols.size() > 8 ? cols[8] : "";
      std::string address = cols.size() > 9 ? cols[9] : "";
      
      trimR(deviceGbId);
      trimR(platformGbIdCol);
      trimR(platformId);
      trimR(platformName);
      trimR(o);
      trimR(manufacturer);
      trimR(model);
      trimR(address);
      
      bool online = (o == "t" || o == "true" || o == "1");
      std::string pidJson = (platformId.empty() || platformId == " ") ? "null" : platformId;
      
      items += "{\"id\":\"" + id + "\",\"deviceGbId\":\"" + escapeJsonString(deviceGbId) +
               "\",\"platformGbId\":\"" + escapeJsonString(platformGbIdCol) + "\",\"name\":\"" +
               escapeJsonString(name) + "\",\"platformDbId\":" + pidJson + ",\"platformName\":\"" +
               escapeJsonString(platformName) + "\",\"online\":" + jsonBool(online) +
               ",\"manufacturer\":\"" + escapeJsonString(manufacturer) + "\",\"model\":\"" +
               escapeJsonString(model) + "\",\"address\":\"" + escapeJsonString(address) + "\"}";
    }
  }
  items += "]";
  std::string body = "{\"code\":0,\"message\":\"ok\",\"data\":{\"items\":" + items + ",\"total\":" + std::to_string(count) + "}}";
  sendJson(body);
}

/**
 * @brief 发送平台目录树（支持树形结构）
 * @details 获取指定平台的完整目录树，包括设备、目录、行政区域
 */
void HttpSession::sendPlatformCatalogTree(const std::string& query) {
  std::string platformId = getQueryParam(query, "platformId");
  if (platformId.empty()) {
    sendJsonError(400, "缺少 platformId 参数");
    return;
  }
  
  // 验证平台是否存在
  std::string checkSql = "SELECT id, gb_id, name FROM device_platforms WHERE id = " + gb::escapeSqlString(platformId);
  std::string checkOut = gb::execPsql(checkSql.c_str());
  if (checkOut.empty()) {
    sendJsonError(404, "平台不存在");
    return;
  }
  
  // 目录节点 + 设备行关联 cameras.online（实时预览树统计与样式）
  std::string sql =
      "SELECT cn.node_id, cn.parent_id, cn.name, cn.node_type, cn.manufacturer, cn.model, cn.status, "
      "cn.civil_code, COALESCE(cn.parental,0), COALESCE(cn.business_group_id,''), "
      "COALESCE(cn.item_num::text,''), (cn.node_type = 0 AND COALESCE(c.online,false)) "
      "FROM catalog_nodes cn "
      "LEFT JOIN cameras c ON c.platform_id = cn.platform_id AND c.device_gb_id = cn.node_id "
      "WHERE cn.platform_id = " +
      gb::escapeSqlString(platformId) + " ORDER BY COALESCE(cn.parent_id,''), cn.node_id";
  std::string out = gb::execPsql(sql.c_str());

  std::map<std::string, std::string> nodeMap;  // node_id -> node json

  if (!out.empty()) {
    std::vector<std::string> lines = split(out, '\n');
    for (const auto& lineRaw : lines) {
      if (lineRaw.empty()) continue;
      std::vector<std::string> cols = split(lineRaw, '|');
      if (cols.size() < 4) continue;

      std::string nodeId = cols[0];
      std::string parentId = cols[1];
      std::string name = cols[2];
      std::string typeStr = cols.size() > 3 ? cols[3] : "0";
      std::string manufacturer = cols.size() > 4 ? cols[4] : "";
      std::string model = cols.size() > 5 ? cols[5] : "";
      std::string status = cols.size() > 6 ? cols[6] : "";
      std::string civilCode = cols.size() > 7 ? cols[7] : "";
      std::string parentalStr = cols.size() > 8 ? cols[8] : "0";
      std::string businessGroupId = cols.size() > 9 ? cols[9] : "";
      std::string itemNumStr = cols.size() > 10 ? cols[10] : "";
      std::string camOnlineRaw = cols.size() > 11 ? cols[11] : "";

      trimR(nodeId);
      trimR(parentId);
      trimR(name);
      trimR(typeStr);
      trimR(manufacturer);
      trimR(model);
      trimR(status);
      trimR(civilCode);
      trimR(parentalStr);
      trimR(businessGroupId);
      trimR(itemNumStr);
      trimR(camOnlineRaw);

      int type = std::atoi(typeStr.c_str());
      std::string typeName = (type == 0) ? "device" : (type == 1) ? "directory" : "region";
      bool isCamera = false;
      if (type == 0 && nodeId.size() >= 13) {
        std::string typeCode3 = nodeId.substr(10, 3);
        isCamera = (typeCode3 == "131" || typeCode3 == "132");
      }
      bool cameraOnline =
          isCamera && (camOnlineRaw == "t" || camOnlineRaw == "true" || camOnlineRaw == "1");

      std::string parentJson =
          parentId.empty() ? "null" : ("\"" + escapeJsonString(parentId) + "\"");
      std::string bgJson =
          businessGroupId.empty() ? "null" : ("\"" + escapeJsonString(businessGroupId) + "\"");
      std::string itemNumJson = "null";
      if (!itemNumStr.empty()) {
        char* endp = nullptr;
        long inum = std::strtol(itemNumStr.c_str(), &endp, 10);
        if (endp != itemNumStr.c_str() && *endp == '\0') {
          itemNumJson = std::to_string(inum);
        }
      }

      std::string nodeJson = std::string("{\"nodeId\":\"") + escapeJsonString(nodeId) +
                            "\",\"parentId\":" + parentJson + ",\"name\":\"" + escapeJsonString(name) +
                            "\",\"type\":" + typeStr + ",\"typeName\":\"" + typeName +
                            "\",\"manufacturer\":\"" + escapeJsonString(manufacturer) +
                            "\",\"model\":\"" + escapeJsonString(model) + "\",\"status\":\"" + status +
                            "\",\"civilCode\":\"" + escapeJsonString(civilCode) + "\",\"parental\":" +
                            parentalStr + ",\"businessGroupId\":" + bgJson + ",\"itemNum\":" + itemNumJson +
                            ",\"isCamera\":" + jsonBool(isCamera) +
                            ",\"cameraOnline\":" + jsonBool(cameraOnline) + "}";

      nodeMap[nodeId] = nodeJson;
    }
  }
  
  // 构建响应（扁平列表，前端自行构建树形）
  std::string items = "[";
  int count = 0;
  for (const auto& pair : nodeMap) {
    if (count > 0) items += ",";
    items += pair.second;
    count++;
  }
  items += "]";
  
  std::string body = "{\"code\":0,\"message\":\"ok\",\"data\":{\"items\":" + items + 
                     ",\"total\":" + std::to_string(count) + "}}";
  sendJson(body);
}

void HttpSession::handleAuthLogin(const std::string& body) {
  std::string username = extractJsonString(body, "username");
  std::string password = extractJsonString(body, "password");
  if (username.empty() || password.empty()) {
    sendJsonError(400, "用户名或密码不能为空");
    return;
  }
  std::string sql = "SELECT id, password_hash FROM users WHERE username='" + gb::escapeSqlString(username) + "'";
  std::string out = gb::execPsql(sql.c_str());
  if (out.empty()) {
    std::string cnt = gb::execPsql("SELECT count(*) FROM users");
    if (!cnt.empty() && cnt[0] == '0') {
      std::string hash = gb::hashPasswordDefault("admin");
      if (!hash.empty()) {
        std::string ins = "INSERT INTO users (username, password_hash, display_name, roles) VALUES ('admin','" + gb::escapeSqlString(hash) + "','管理员','admin')";
        gb::execPsqlCommand(ins);
        username = "admin";
        out = gb::execPsql("SELECT id, password_hash FROM users WHERE username='admin'");
      }
    }
  }
  if (out.empty()) {
    sendJsonError(401, "用户名或密码错误");
    return;
  }
  auto pos = out.find('\n');
  std::string firstLine = (pos == std::string::npos) ? out : out.substr(0, pos);
  std::vector<std::string> cols = split(firstLine, '|');
  if (cols.size() < 2) {
    sendJsonError(401, "用户名或密码错误");
    return;
  }
  std::string dbHash = cols[1];
  while (!dbHash.empty() && (dbHash.back() == '\n' || dbHash.back() == '\r' || dbHash.back() == ' ')) dbHash.pop_back();
  std::string inputHash = gb::hashPasswordDefault(password);
  if (inputHash.empty() || inputHash != dbHash) {
    sendJsonError(401, "用户名或密码错误");
    return;
  }
  std::string token = gb::generateToken();
  if (token.empty()) {
    sendJsonError(500, "生成令牌失败");
    return;
  }
  std::string insToken = "INSERT INTO user_tokens (user_id, token) VALUES (" + cols[0] + ", '" + gb::escapeSqlString(token) + "')";
  if (!gb::execPsqlCommand(insToken)) {
    sendJsonError(500, "登录失败");
    return;
  }
  std::string displayName = "管理员";
  std::string roles = "admin";
  std::string res = "{\"code\":0,\"message\":\"ok\",\"data\":{";
  res += "\"token\":\"" + token + "\",";
  res += "\"user\":{";
  res += "\"username\":\"" + username + "\",";
  res += "\"displayName\":\"" + displayName + "\",";
  res += "\"roles\":[\"" + roles + "\"]";
  res += "}}}";
  sendJson(res);
}

void HttpSession::handleAuthMe(const std::string& authHeader) {
  if (authHeader.size() < 8 || authHeader.substr(0, 7) != "Bearer ") {
    sendJsonError(401, "未提供有效令牌");
    return;
  }
  std::string token = authHeader.substr(7);
  if (token.empty()) {
    sendJsonError(401, "未提供有效令牌");
    return;
  }
  std::string sql = "SELECT user_id FROM user_tokens WHERE token='" + gb::escapeSqlString(token) + "'";
  std::string out = gb::execPsql(sql.c_str());
  if (out.empty()) {
    sendJsonError(401, "未提供有效令牌");
    return;
  }
  auto pos = out.find('\n');
  std::string firstLine = (pos == std::string::npos) ? out : out.substr(0, pos);
  std::vector<std::string> cols = split(firstLine, '|');
  if (cols.empty()) {
    sendJsonError(401, "未提供有效令牌");
    return;
  }
  std::string userSql = "SELECT username, display_name, roles FROM users WHERE id=" + cols[0];
  std::string userOut = gb::execPsql(userSql.c_str());
  if (userOut.empty()) {
    sendJsonError(401, "用户不存在");
    return;
  }
  pos = userOut.find('\n');
  firstLine = (pos == std::string::npos) ? userOut : userOut.substr(0, pos);
  cols = split(firstLine, '|');
  std::string uname = cols.size() > 0 ? cols[0] : "";
  std::string dname = cols.size() > 1 ? cols[1] : "";
  std::string roles = cols.size() > 2 ? cols[2] : "admin";
  std::string res = "{\"code\":0,\"message\":\"ok\",\"data\":{";
  res += "\"username\":\"" + uname + "\",";
  res += "\"displayName\":\"" + dname + "\",";
  res += "\"roles\":[\"" + roles + "\"]}}";
  sendJson(res);
}

void HttpSession::handleAuthLogout(const std::string& authHeader) {
  if (authHeader.size() < 8 || authHeader.substr(0, 7) != "Bearer ") {
    sendJson("{\"code\":0,\"message\":\"ok\",\"data\":null}");
    return;
  }
  std::string token = authHeader.substr(7);
  if (!token.empty()) {
    std::string sql = "DELETE FROM user_tokens WHERE token='" + gb::escapeSqlString(token) + "'";
    gb::execPsqlCommand(sql);
  }
  sendJson("{\"code\":0,\"message\":\"ok\",\"data\":null}");
}

void HttpSession::handleAuthChangePassword(const std::string& body, const std::string& authHeader) {
  if (authHeader.size() < 8 || authHeader.substr(0, 7) != "Bearer ") {
    sendJsonError(401, "未提供有效令牌");
    return;
  }
  std::string token = authHeader.substr(7);
  std::string sql = "SELECT user_id FROM user_tokens WHERE token='" + gb::escapeSqlString(token) + "'";
  std::string out = gb::execPsql(sql.c_str());
  if (out.empty()) {
    sendJsonError(401, "未提供有效令牌");
    return;
  }
  auto pos = out.find('\n');
  std::string firstLine = (pos == std::string::npos) ? out : out.substr(0, pos);
  std::vector<std::string> cols = split(firstLine, '|');
  if (cols.empty()) {
    sendJsonError(401, "未提供有效令牌");
    return;
  }
  std::string userId = cols[0];
  std::string oldPass = extractJsonString(body, "oldPassword");
  std::string newPass = extractJsonString(body, "newPassword");
  if (oldPass.empty() || newPass.empty()) {
    sendJsonError(400, "原密码和新密码不能为空");
    return;
  }
  std::string userSql = "SELECT password_hash FROM users WHERE id=" + userId;
  std::string userOut = gb::execPsql(userSql.c_str());
  if (userOut.empty()) {
    sendJsonError(401, "用户不存在");
    return;
  }
  pos = userOut.find('\n');
  firstLine = (pos == std::string::npos) ? userOut : userOut.substr(0, pos);
  std::string dbHash = split(firstLine, '|')[0];
  if (gb::hashPasswordDefault(oldPass) != dbHash) {
    sendJsonError(401, "原密码不正确");
    return;
  }
  std::string newHash = gb::hashPasswordDefault(newPass);
  if (newHash.empty()) {
    sendJsonError(500, "密码加密失败");
    return;
  }
  std::string upd = "UPDATE users SET password_hash='" + gb::escapeSqlString(newHash) + "', updated_at=CURRENT_TIMESTAMP WHERE id=" + userId;
  if (!gb::execPsqlCommand(upd)) {
    sendJsonError(500, "修改失败");
    return;
  }
  std::string delToken = "DELETE FROM user_tokens WHERE token='" + gb::escapeSqlString(token) + "'";
  gb::execPsqlCommand(delToken);
  sendJson("{\"code\":0,\"message\":\"ok\",\"data\":null}");
}

void HttpSession::handlePutConfigLocalGb(const std::string& body) {
  std::string gbId = extractJsonString(body, "gbId");
  std::string domain = extractJsonString(body, "domain");
  std::string name = extractJsonString(body, "name");
  std::string username = extractJsonString(body, "username");
  std::string password = extractJsonString(body, "password");
   std::string signalIp = extractJsonString(body, "signalIp");
   int signalPort = extractJsonInt(body, "signalPort");
  bool udp = true;
  bool tcp = false;
  if (!extractTransportObjectBools(body, udp, tcp)) {
    udp = extractJsonBool(body, "udp");
    tcp = extractJsonBool(body, "tcp");
  }
  std::string sql = "INSERT INTO gb_local_config (id, gb_id, domain, name, username, password, signal_ip, signal_port, transport_udp, transport_tcp) VALUES (1, '" +
      gb::escapeSqlString(gbId) + "','" + gb::escapeSqlString(domain) + "','" + gb::escapeSqlString(name) + "','" +
      gb::escapeSqlString(username) + "','" + gb::escapeSqlString(password) + "','" + gb::escapeSqlString(signalIp) + "'," +
      std::to_string(signalPort) + "," + jsonBool(udp) + "," + jsonBool(tcp) +
      ") ON CONFLICT (id) DO UPDATE SET gb_id=EXCLUDED.gb_id, domain=EXCLUDED.domain, name=EXCLUDED.name, username=EXCLUDED.username, password=EXCLUDED.password, signal_ip=EXCLUDED.signal_ip, signal_port=EXCLUDED.signal_port, transport_udp=EXCLUDED.transport_udp, transport_tcp=EXCLUDED.transport_tcp, updated_at=CURRENT_TIMESTAMP";
  if (!gb::execPsqlCommand(sql)) {
    sendJsonError(500, "保存失败");
    return;
  }
  /* gb_local_config 已写入库：刷新 SIP 侧内存缓存（realm/本机 ID/鉴权密码等），否则设备 Digest 仍用旧密码 */
  gb::reloadSystemConfigCache();
  sendJson("{\"code\":0,\"message\":\"ok\",\"data\":null}");
}

void HttpSession::handlePutConfigMedia(const std::string& body) {
  gb::execPsqlCommand(
      "ALTER TABLE media_config ADD COLUMN IF NOT EXISTS preview_invite_timeout_sec INTEGER DEFAULT 45");
  if (!gb::MediaService::ensureMediaApiUrlMigration()) {
    sendJsonError(500, "数据库迁移失败");
    return;
  }
  int start = extractJsonInt(body, "start");
  int end = extractJsonInt(body, "end");
  std::string playbackHost = extractJsonString(body, "playbackHost");
  std::string mediaApiUrlRaw = extractJsonString(body, "mediaApiUrl");
  if (start <= 0) start = 30000;
  if (end <= 0) end = 30500;
  if (start > end) {
    sendJsonError(400, "起始端口不能大于结束端口");
    return;
  }
  if (playbackHost.empty()) {
    sendJsonError(400, "缺少 playbackHost");
    return;
  }
  if (mediaApiUrlRaw.empty()) {
    sendJsonError(400, "缺少 mediaApiUrl");
    return;
  }
  std::string mediaApiUrlNorm = gb::MediaService::normalizeMediaApiUrl(mediaApiUrlRaw);
  if (mediaApiUrlNorm.empty()) {
    sendJsonError(400, "mediaApiUrl 无效");
    return;
  }

  // 先保存配置到数据库（不依赖ZLM连接检测结果）
  std::string exists = gb::execPsql("SELECT 1 FROM media_config WHERE id = 1 LIMIT 1");
  bool hasRow = !exists.empty();
  std::string sql;
  std::string rtpTr = extractJsonString(body, "rtpTransport");
  if (rtpTr != "tcp") rtpTr = "udp";

  bool setPreviewTimeout = false;
  int previewTimeoutSec = 0;
  if (body.find("\"previewInviteTimeoutSec\"") != std::string::npos) {
    previewTimeoutSec = extractJsonInt(body, "previewInviteTimeoutSec");
    if (previewTimeoutSec > 0) {
      if (previewTimeoutSec < 10) {
        previewTimeoutSec = 10;
      }
      if (previewTimeoutSec > 600) {
        previewTimeoutSec = 600;
      }
      setPreviewTimeout = true;
    }
  }

  if (hasRow) {
    sql = "UPDATE media_config SET rtp_port_start=" + std::to_string(start) +
          ", rtp_port_end=" + std::to_string(end) +
          ", media_http_host='" + gb::escapeSqlString(playbackHost) + "'" +
          ", media_api_url='" + gb::escapeSqlString(mediaApiUrlNorm) + "'" +
          ", rtp_transport='" + gb::escapeSqlString(rtpTr) + "'";
    if (setPreviewTimeout) {
      sql += ", preview_invite_timeout_sec=" + std::to_string(previewTimeoutSec);
    }
    sql += ", updated_at=CURRENT_TIMESTAMP WHERE id=1";
  } else {
    int insPreview = setPreviewTimeout ? previewTimeoutSec : 45;
    sql = "INSERT INTO media_config (id, rtp_port_start, rtp_port_end, media_http_host, media_api_url, zlm_secret, rtp_transport, preview_invite_timeout_sec) VALUES (1, " +
          std::to_string(start) + "," + std::to_string(end) + ",'" + gb::escapeSqlString(playbackHost) +
          "','" + gb::escapeSqlString(mediaApiUrlNorm) + "','','" + gb::escapeSqlString(rtpTr) + "'," +
          std::to_string(insPreview) + ")";
  }
  if (!gb::execPsqlCommand(sql)) {
    sendJsonError(500, "保存失败");
    return;
  }
  gb::GetMediaService().refreshZlmRuntimeConfig();

  // 保存成功后，异步检测ZLM连接状态（不影响保存结果）
  std::string secret = gb::GetMediaService().getZlmSecret();
  std::string zlmErr;
  bool zlmConnected = gb::MediaService::pushRtpProxyPortRangeToZlm(mediaApiUrlNorm, secret, start, end, zlmErr);

  // 返回保存结果和ZLM检测状态
  std::string response = "{\"code\":0,\"message\":\"ok\",\"data\":{\"saved\":true,\"zlmCheck\":{";
  response += "\"connected\":" + std::string(zlmConnected ? "true" : "false") + ",";
  response += "\"message\":\"" + escapeJsonString(zlmConnected ? "流媒体连接正常" : ("流媒体连接失败: " + zlmErr)) + "\"";
  response += "}}}";
  sendJson(response);
}

void HttpSession::handlePostPlatform(const std::string& body) {
  std::string name = extractJsonField(body, "name");
  std::string sipDomain = extractJsonField(body, "sipDomain");
  std::string gbId = extractJsonField(body, "gbId");
  std::string sipIp = extractJsonField(body, "sipIp");
  int sipPort = std::atoi(extractJsonField(body, "sipPort").c_str());
  std::string transport = extractJsonField(body, "transport");
  std::string regUsername = extractJsonField(body, "regUsername");
  std::string regPassword = extractJsonField(body, "regPassword");
  if (name.empty() || sipDomain.empty() || gbId.empty() || sipIp.empty()) {
    sendJsonError(400, "缺少必填字段");
    return;
  }
  if (!upstreamGbIdValid(gbId)) {
    sendJsonError(400, "上级平台国标 ID 须为 20 位数字");
    return;
  }
  if (!upstreamDomainValid(sipDomain)) {
    sendJsonError(400, "上级平台域格式无效（1～32 位字母数字或 .-_）");
    return;
  }
  if (transport.empty()) transport = "udp";
  for (char& ch : transport) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  if (transport != "udp" && transport != "tcp") {
    sendJsonError(400, "transport 仅支持 udp 或 tcp");
    return;
  }
  if (sipPort < 1 || sipPort > 65535) {
    sendJsonError(400, "sipPort 须在 1～65535");
    return;
  }
  int registerExpires = std::atoi(extractJsonField(body, "registerExpires").c_str());
  if (registerExpires <= 0) registerExpires = 3600;
  if (registerExpires < 60 || registerExpires > 86400) {
    sendJsonError(400, "registerExpires 须在 60～86400 秒");
    return;
  }
  int heartbeatInterval = std::atoi(extractJsonField(body, "heartbeatInterval").c_str());
  if (heartbeatInterval <= 0) heartbeatInterval = 60;
  if (heartbeatInterval < 10 || heartbeatInterval > 3600) {
    sendJsonError(400, "heartbeatInterval 须在 10～3600 秒");
    return;
  }
  if (upstreamPlatformGbIdTaken(gbId, nullptr)) {
    sendJsonError(409, "上级平台国标 ID 已存在");
    return;
  }
  bool enabled = true;
  if (body.find("\"enabled\":") != std::string::npos) enabled = extractJsonBool(body, "enabled");
  std::string sql =
      "INSERT INTO upstream_platforms (name, sip_domain, gb_id, sip_ip, sip_port, transport, reg_username, "
      "reg_password, enabled, register_expires, heartbeat_interval) VALUES ('" +
      gb::escapeSqlString(name) + "','" + gb::escapeSqlString(sipDomain) + "','" + gb::escapeSqlString(gbId) + "','" +
      gb::escapeSqlString(sipIp) + "'," + std::to_string(sipPort) + ",'" + gb::escapeSqlString(transport) + "','" +
      gb::escapeSqlString(regUsername) + "','" + gb::escapeSqlString(regPassword) + "'," + jsonBool(enabled) + "," +
      std::to_string(registerExpires) + "," + std::to_string(heartbeatInterval) + ") RETURNING id";
  std::string newId = gb::execPsql(sql.c_str());
  if (newId.empty()) {
    sendJsonError(500, "新增失败");
    return;
  }
  std::string idTrim = newId;
  size_t nl = idTrim.find('\n');
  if (nl != std::string::npos) idTrim = idTrim.substr(0, nl);
  while (!idTrim.empty() && (idTrim.back() == '\r' || idTrim.back() == ' ')) idTrim.pop_back();
  if (idTrim.empty()) {
    sendJsonError(500, "新增失败");
    return;
  }
  long long newRowId = std::atoll(idTrim.c_str());
  if (newRowId > 0) gb::requestUpstreamRegistrarSyncRow(newRowId);
  sendJson("{\"code\":0,\"message\":\"ok\",\"data\":{\"id\":" + idTrim + "}}");
}

void HttpSession::handlePutPlatform(const std::string& idStr, const std::string& body) {
  for (size_t i = 0; i < idStr.size(); ++i) {
    if (idStr[i] < '0' || idStr[i] > '9') {
      sendJsonError(400, "无效的平台 ID");
      return;
    }
  }
  std::string name = extractJsonField(body, "name");
  std::string sipDomain = extractJsonField(body, "sipDomain");
  std::string gbId = extractJsonField(body, "gbId");
  std::string sipIp = extractJsonField(body, "sipIp");
  int sipPort = std::atoi(extractJsonField(body, "sipPort").c_str());
  std::string transport = extractJsonField(body, "transport");
  std::string regUsername = extractJsonField(body, "regUsername");
  bool hasRegPasswordField = body.find("\"regPassword\":") != std::string::npos;
  std::string regPassword = extractJsonField(body, "regPassword");
  if (trimWhitespace(body).empty()) {
    sendJsonError(400, "请求体不能为空");
    return;
  }
  if (!gbId.empty() && !upstreamGbIdValid(gbId)) {
    sendJsonError(400, "上级平台国标 ID 须为 20 位数字");
    return;
  }
  if (!gbId.empty() && upstreamPlatformGbIdTaken(gbId, idStr.c_str())) {
    sendJsonError(409, "上级平台国标 ID 已存在");
    return;
  }
  if (!sipDomain.empty() && !upstreamDomainValid(sipDomain)) {
    sendJsonError(400, "上级平台域格式无效");
    return;
  }
  for (char& ch : transport) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  if (!transport.empty() && transport != "udp" && transport != "tcp") {
    sendJsonError(400, "transport 仅支持 udp 或 tcp");
    return;
  }
  if (sipPort != 0 && (sipPort < 1 || sipPort > 65535)) {
    sendJsonError(400, "sipPort 须在 1～65535");
    return;
  }
  if (sipPort <= 0) sipPort = 5060;
  if (transport.empty()) transport = "udp";
  bool enabled = true;
  if (body.find("\"enabled\":") != std::string::npos) enabled = extractJsonBool(body, "enabled");
  std::string sql = "UPDATE upstream_platforms SET ";
  std::vector<std::string> sets;
  if (!name.empty()) sets.push_back("name='" + gb::escapeSqlString(name) + "'");
  if (!sipDomain.empty()) sets.push_back("sip_domain='" + gb::escapeSqlString(sipDomain) + "'");
  if (!gbId.empty()) sets.push_back("gb_id='" + gb::escapeSqlString(gbId) + "'");
  if (!sipIp.empty()) sets.push_back("sip_ip='" + gb::escapeSqlString(sipIp) + "'");
  sets.push_back("sip_port=" + std::to_string(sipPort));
  sets.push_back("transport='" + gb::escapeSqlString(transport) + "'");
  sets.push_back("reg_username='" + gb::escapeSqlString(regUsername) + "'");
  if (hasRegPasswordField)
    sets.push_back("reg_password='" + gb::escapeSqlString(regPassword) + "'");
  sets.push_back("enabled=" + jsonBool(enabled));
  int registerExpires = std::atoi(extractJsonField(body, "registerExpires").c_str());
  if (registerExpires != 0 && (registerExpires < 60 || registerExpires > 86400)) {
    sendJsonError(400, "registerExpires 须在 60～86400 秒");
    return;
  }
  if (registerExpires >= 60 && registerExpires <= 86400)
    sets.push_back("register_expires=" + std::to_string(registerExpires));
  int heartbeatInterval = std::atoi(extractJsonField(body, "heartbeatInterval").c_str());
  if (heartbeatInterval != 0 && (heartbeatInterval < 10 || heartbeatInterval > 3600)) {
    sendJsonError(400, "heartbeatInterval 须在 10～3600 秒");
    return;
  }
  if (heartbeatInterval >= 10 && heartbeatInterval <= 3600)
    sets.push_back("heartbeat_interval=" + std::to_string(heartbeatInterval));
  sets.push_back("updated_at=CURRENT_TIMESTAMP");
  for (size_t i = 0; i < sets.size(); ++i) {
    if (i > 0) sql += ",";
    sql += sets[i];
  }
  sql += " WHERE id=" + idStr;
  if (!gb::execPsqlCommand(sql)) {
    sendJsonError(500, "更新失败");
    return;
  }
  long long pid = std::atoll(idStr.c_str());
  if (pid > 0) gb::requestUpstreamRegistrarSyncRow(pid);
  sendJson("{\"code\":0,\"message\":\"ok\",\"data\":null}");
}

void HttpSession::handleDeletePlatform(const std::string& idStr) {
  for (size_t i = 0; i < idStr.size(); ++i) {
    if (idStr[i] < '0' || idStr[i] > '9') {
      sendJsonError(400, "无效的平台 ID");
      return;
    }
  }
  std::string sql = "DELETE FROM upstream_platforms WHERE id=" + idStr;
  if (!gb::execPsqlCommand(sql)) {
    sendJsonError(500, "删除失败");
    return;
  }
  long long did = std::atoll(idStr.c_str());
  if (did > 0) gb::requestUpstreamRegistrarRemoveRow(did);
  sendJson("{\"code\":0,\"message\":\"ok\",\"data\":null}");
}

void HttpSession::handlePutPlatformCatalogScope(const std::string& idStr, const std::string& body) {
  for (size_t i = 0; i < idStr.size(); ++i) {
    if (idStr[i] < '0' || idStr[i] > '9') {
      sendJsonError(400, "无效的平台 ID");
      return;
    }
  }
  std::vector<long long> nodeIds;
  parseCatalogGroupNodeIdsArray(body, nodeIds);
  std::vector<long long> exclCamIds;
  parseExcludedCameraIdsArray(body, exclCamIds);
  if (!gb::execPsqlCommand("DELETE FROM upstream_catalog_scope WHERE upstream_platform_id=" + idStr)) {
    sendJsonError(500, "清空目录范围失败");
    return;
  }
  for (long long nid : nodeIds) {
    std::string ins =
        "INSERT INTO upstream_catalog_scope (upstream_platform_id, catalog_group_node_id) VALUES (" + idStr +
        "," + std::to_string(nid) + ")";
    if (!gb::execPsqlCommand(ins)) {
      sendJsonError(500, "保存目录范围失败");
      return;
    }
  }
  if (!gb::execPsqlCommand("DELETE FROM upstream_catalog_camera_exclude WHERE upstream_platform_id=" + idStr)) {
    sendJsonError(500, "清空摄像头排除列表失败");
    return;
  }
  for (long long cid : exclCamIds) {
    std::string ins =
        "INSERT INTO upstream_catalog_camera_exclude (upstream_platform_id, camera_id) VALUES (" + idStr + "," +
        std::to_string(cid) + ")";
    if (!gb::execPsqlCommand(ins)) {
      sendJsonError(500, "保存摄像头排除失败");
      return;
    }
  }
  sendJson("{\"code\":0,\"message\":\"ok\",\"data\":null}");
}

void HttpSession::handlePostPlatformCatalogNotify(const std::string& idStr) {
  for (size_t i = 0; i < idStr.size(); ++i) {
    if (idStr[i] < '0' || idStr[i] > '9') {
      sendJsonError(400, "无效的平台 ID");
      return;
    }
  }
  int64_t pid = std::atoll(idStr.c_str());
  if (!gb::enqueueUpstreamCatalogNotify(pid)) {
    sendJsonError(400, "入队目录上报失败");
    return;
  }
  InfoL << "【上级目录上报】HTTP 已入队 upstreamId=" << pid;
  sendJson("{\"code\":0,\"message\":\"ok\",\"data\":null}");
}

static bool parseCatalogNodeId(const std::string& idStr, std::string& out) {
  if (idStr == "root" || idStr.empty()) return false;
  for (size_t i = 0; i < idStr.size(); ++i) {
    if (idStr[i] < '0' || idStr[i] > '9') return false;
  }
  out = idStr;
  return true;
}

void HttpSession::handlePostCatalogNode(const std::string& body) {
  (void)body;
  sendJsonError(410, "已废弃：请使用 POST /api/catalog-group/nodes 维护本机目录编组");
}

void HttpSession::handlePutCatalogNode(const std::string& idStr, const std::string& body) {
  std::string safeId;
  if (!parseCatalogNodeId(idStr, safeId)) {
    sendJsonError(400, "无效的节点 ID");
    return;
  }
  std::string name = extractJsonString(body, "name");
  if (name.empty()) {
    sendJsonError(400, "节点名称不能为空");
    return;
  }
  std::string code = extractJsonString(body, "code");
  int level = extractJsonInt(body, "level");
  std::string sql = "UPDATE catalog_nodes SET name='" + gb::escapeSqlString(name) + "', code='" + gb::escapeSqlString(code) + "', level=" + std::to_string(level) + ", updated_at=CURRENT_TIMESTAMP WHERE id=" + safeId;
  if (!gb::execPsqlCommand(sql)) {
    sendJsonError(500, "更新失败");
    return;
  }
  sendJson("{\"code\":0,\"message\":\"ok\",\"data\":null}");
}

void HttpSession::handleDeleteCatalogNode(const std::string& idStr) {
  std::string safeId;
  if (!parseCatalogNodeId(idStr, safeId)) {
    sendJsonError(400, "无效的节点 ID");
    return;
  }
  std::string sql = "DELETE FROM catalog_nodes WHERE id=" + safeId;
  if (!gb::execPsqlCommand(sql)) {
    sendJsonError(500, "删除失败");
    return;
  }
  sendJson("{\"code\":0,\"message\":\"ok\",\"data\":null}");
}

void HttpSession::handlePutCatalogNodeCameras(const std::string& idStr, const std::string& body) {
  std::string safeId;
  if (!parseCatalogNodeId(idStr, safeId)) {
    sendJsonError(400, "无效的节点 ID");
    return;
  }
  std::vector<std::string> ids;
  extractJsonCameraIds(body, ids);
  if (!gb::execPsqlCommand("DELETE FROM catalog_node_cameras WHERE node_id=" + safeId)) {
    sendJsonError(500, "保存失败");
    return;
  }
  for (const auto& cid : ids) {
    if (cid.empty() || !isNumericId(cid)) continue;
    std::string ins =
        "INSERT INTO catalog_node_cameras (node_id, camera_id) VALUES (" + safeId + "," + cid + ")";
    gb::execPsqlCommand(ins);
  }
  sendJson("{\"code\":0,\"message\":\"ok\",\"data\":null}");
}

void HttpSession::handleCatalogQuery(const std::string& platformId) {
  // 验证平台 ID 是否有效并获取平台 GBID
  std::string sql = "SELECT gb_id FROM device_platforms WHERE id = " + gb::escapeSqlString(platformId);
  std::string out = gb::execPsql(sql.c_str());
  
  if (out.empty()) {
    sendJsonError(404, "平台不存在");
    return;
  }
  
  size_t nl = out.find('\n');
  std::string platformGbId = (nl == std::string::npos) ? out : out.substr(0, nl);
  // 清理换行符
  while (!platformGbId.empty() && (platformGbId.back() == '\r' || platformGbId.back() == '\n' || platformGbId.back() == ' ')) {
    platformGbId.pop_back();
  }
  
  if (platformGbId.empty()) {
    sendJsonError(404, "平台不存在");
    return;
  }
  
  // 页面手动点击应每次都真正触发，使用毫秒级序列号避免同秒重复。
  auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  int sn = static_cast<int>(nowMs & 0x7fffffff);
  
  // 手动查询目录应每次都触发，不受自动注册去重限制。
  if (gb::sendCatalogQueryAsync(platformGbId, sn, true)) {
    InfoL << "Catalog query initiated for platform " << platformGbId << " (SN=" << sn << ")";
    std::string res = "{\"code\":0,\"message\":\"设备目录查询请求已加入队列\",\"data\":{\"platformGbId\":\"" + platformGbId + "\",\"sn\":" + std::to_string(sn) + "}}";
    sendJson(res);
  } else {
    sendJsonError(500, "发送 Catalog 查询请求失败");
  }
}

// ========== 视频预览（点播）API实现 ==========

void HttpSession::handlePreviewStart(const std::string& cameraId, const std::string& body) {
  InfoL << "【handlePreviewStart】Preview request for camera " << cameraId << ", body: " << body;

  std::string platformGbId = resolvePlatformGbIdFromBody(body);
  InfoL << "【handlePreviewStart】Resolved platformGbId from request: " << platformGbId;

  std::string dbId;
  std::string deviceGb;
  std::string platGbFromRow;
  if (!gb::resolveCameraRowByPathSegment(cameraId, platformGbId, dbId, deviceGb, platGbFromRow)) {
    sendJsonError(404, "摄像头不存在或多平台同号未传 platformGbId");
    return;
  }

  std::string checkSql = "SELECT c.name, p.gb_id, p.online FROM cameras c JOIN device_platforms p ON p.id = c.platform_id "
                         "WHERE c.id = " +
                         dbId + " LIMIT 1";
  InfoL << "【handlePreviewStart】SQL: " << checkSql;
  std::string checkOut = gb::execPsql(checkSql.c_str());

  if (checkOut.empty()) {
    sendJsonError(404, "摄像头不存在");
    return;
  }

  size_t nl = checkOut.find('\n');
  std::string line = (nl == std::string::npos) ? checkOut : checkOut.substr(0, nl);

  std::vector<std::string> cols;
  std::string cur;
  for (char c : line) {
    if (c == '|') {
      cols.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  cols.push_back(cur);

  if (cols.size() < 3) {
    sendJsonError(500, "数据库查询错误");
    return;
  }

  std::string cameraName = cols[0];
  std::string platformGbIdFromDb = cols[1];
  bool platformOnline = (cols[2] == "t" || cols[2] == "true" || cols[2] == "1");

  if (!platformOnline) {
    sendJsonError(4001, "设备离线，无法预览");
    return;
  }
  
  // 初始化MediaService（如果未初始化）
  MediaService& mediaSvc = GetMediaService();
  if (!mediaSvc.initialize()) {
    sendJsonError(4003, "媒体服务初始化失败");
    return;
  }

  const std::string streamId = MediaService::buildStreamId(platformGbIdFromDb, deviceGb);
  InfoL << "【handlePreviewStart】Computed streamId: " << streamId;
  auto buildPreviewResponse = [](const std::shared_ptr<StreamSession>& session, const std::string& message) {
    return std::string("{")
        + "\"code\":0,"
        + "\"message\":\"" + escapeJsonString(message) + "\","
        + "\"data\":{"
        + "\"sessionId\":\"" + escapeJsonString(session->sessionId) + "\","
        + "\"streamId\":\"" + escapeJsonString(session->streamId) + "\","
        + "\"status\":\"" + getStreamStatusName(session->status) + "\","
        + "\"errorMessage\":\"" + escapeJsonString(session->errorMessage) + "\","
        + "\"flvUrl\":\"" + escapeJsonString(session->flvUrl) + "\","
        + "\"wsFlvUrl\":\"" + escapeJsonString(session->wsFlvUrl) + "\""
        + "}"
        + "}";
  };

  // 检查ZLM中是否已存在该流（设备正在推流）
  bool streamExistsInZlm = mediaSvc.isStreamExistsInZlm(streamId);

  // 检查本地是否已有该流会话
  auto existingSession = mediaSvc.getSessionByStreamId(streamId);
  if (streamExistsInZlm) {
    if (existingSession && existingSession->status != StreamSessionStatus::CLOSED) {
      InfoL << "【handlePreviewStart】Stream exists in ZLM and local session found for stream " << streamId
            << ", returning existing session " << existingSession->sessionId;
      sendJson(buildPreviewResponse(existingSession, "预览会话已存在"));
      return;
    }

    auto attachedSession = mediaSvc.attachToExistingStream(deviceGb, platformGbIdFromDb, platformGbIdFromDb, dbId);
    if (!attachedSession) {
      sendJsonError(500, "附着已有流会话失败");
      return;
    }

    InfoL << "【handlePreviewStart】Stream exists in ZLM and attached local session for stream " << streamId
          << ", session " << attachedSession->sessionId;
    sendJson(buildPreviewResponse(attachedSession, "复用已存在流"));
    return;
  }

  if (existingSession && existingSession->status != StreamSessionStatus::CLOSED) {
    // 本地有会话但ZLM中没有流（会话已过期），清理旧会话
    InfoL << "【handlePreviewStart】Local session exists but no stream in ZLM for stream " << streamId
          << ", closing old session and creating new one";
    mediaSvc.closeSession(existingSession->sessionId);
  }

  // 创建新的流会话
  auto session = mediaSvc.createSession(deviceGb, platformGbIdFromDb, platformGbIdFromDb, dbId);
  if (!session) {
    sendJsonError(500, "创建流会话失败");
    return;
  }

  std::string sdpConnIp;
  bool previewUseTcp = false;
  resolvePreviewStreamParams(platformGbIdFromDb, sdpConnIp, previewUseTcp);
  const int zlmTcpMode = previewUseTcp ? 1 : 0;

  // 调用ZLM openRtpServer（tcp_mode 与 INVITE SDP 一致）
  uint16_t zlmPort = 0;
  if (!mediaSvc.openRtpServer(session->streamId, zlmPort, zlmTcpMode)) {
    mediaSvc.closeSession(session->sessionId);
    sendJsonError(4003, "媒体服务不可用，无法创建收流端口");
    return;
  }

  InfoL << "【preview SDP 对拍】streamId=" << session->streamId << " INVITE SDP c=IN IP4 " << sdpConnIp
        << " m=video " << zlmPort << " (应与 ZLM openRtpServer 端口一致) transport="
        << (previewUseTcp ? "TCP" : "UDP") << " zlm_tcp_mode=" << zlmTcpMode;

  session->zlmPort = zlmPort;
  session->status = StreamSessionStatus::INVITING;
  
  // 发送SIP INVITE点播请求（使用异步版本，由PJSIP工作线程实际发送）
  std::string callId;
  bool inviteResult =
      gb::sendPlayInviteAsync(platformGbIdFromDb, deviceGb, zlmPort, callId, sdpConnIp, previewUseTcp);

  if (!inviteResult) {
    mediaSvc.closeRtpServer(session->streamId);
    mediaSvc.closeSession(session->sessionId);
    sendJsonError(4002, "发送点播请求失败");
    return;
  }

  session->callId = callId;
  mediaSvc.setSessionCallId(session->sessionId, callId);

  InfoL << "【handlePreviewStart】Invite queued, Call-ID: " << callId
        << " (will be sent by PJSIP worker thread)";
  
  // 返回成功响应（流正在建立中）
  InfoL << "【handlePreviewStart】Preview started for cameraDbId=" << dbId << " deviceGb=" << deviceGb
        << ", session " << session->sessionId;
  sendJson(buildPreviewResponse(session, "ok"));
}

void HttpSession::handlePreviewStop(const std::string& cameraId, const std::string& body) {
  InfoL << "【handlePreviewStop】Stop preview request for camera " << cameraId << ", body: " << body;
  
  MediaService& mediaSvc = GetMediaService();
  const std::string platformGbId = resolvePlatformGbIdFromBody(body);
  std::string dbId;
  std::string deviceGb;
  std::string platGbRow;
  std::string streamId;
  if (gb::resolveCameraRowByPathSegment(cameraId, platformGbId, dbId, deviceGb, platGbRow)) {
    streamId = MediaService::buildStreamId(platGbRow, deviceGb);
  } else if (!platformGbId.empty()) {
    streamId = MediaService::buildStreamId(platformGbId, cameraId);
  } else {
    WarnL << "【handlePreviewStop】无法解析摄像头，未传 platformGbId 且路径无法映射到库内行";
    sendJson("{\"code\":0,\"message\":\"ok\",\"data\":null}");
    return;
  }
  InfoL << "【handlePreviewStop】Resolved platformGbId(body)=" << platformGbId << ", streamId: " << streamId;
  
  // 查找会话
  auto session = mediaSvc.getSessionByStreamId(streamId);
  if (!session) {
    WarnL << "【handlePreviewStop】Session not found for streamId " << streamId;
    sendJson("{\"code\":0,\"message\":\"ok\",\"data\":null}");
    return;
  }
  
  // 发送SIP BYE（使用异步版本，由PJSIP工作线程实际发送）
  // 注意：BYE 的 channelId 必须继续使用原始摄像头ID，与 INVITE 保持一致。
  if (!session->callId.empty()) {
    gb::sendPlayByeAsync(session->callId, session->deviceGbId, session->cameraId);
    InfoL << "【handlePreviewStop】BYE queued for call " << session->callId
          << " channel " << session->cameraId;
  }
  
  // 关闭ZLM端口和会话
  mediaSvc.closeSession(session->sessionId);
  
  sendJson("{\"code\":0,\"message\":\"ok\",\"data\":null}");
}

void HttpSession::sendPreviewSessionStatus(const std::string& cameraId, const std::string& query) {
  std::string platformGbId = getQueryParam(query, "platformGbId");
  if (platformGbId.empty()) {
    std::string pid = getQueryParam(query, "platformId");
    if (pid.empty()) {
      pid = getQueryParam(query, "platformDbId");
    }
    if (!pid.empty()) {
      platformGbId = queryPlatformGbIdByPlatformId(pid);
    }
  }

  MediaService& mediaSvc = GetMediaService();
  std::string dbId;
  std::string deviceGb;
  std::string platGbRow;
  std::string streamId;
  if (gb::resolveCameraRowByPathSegment(cameraId, platformGbId, dbId, deviceGb, platGbRow)) {
    streamId = MediaService::buildStreamId(platGbRow, deviceGb);
  } else if (!platformGbId.empty()) {
    streamId = MediaService::buildStreamId(platformGbId, cameraId);
  } else {
    sendJson("{\"code\":0,\"message\":\"ok\",\"data\":null}");
    return;
  }
  auto session = mediaSvc.getSessionByStreamId(streamId);
  if (!session) {
    sendJson("{\"code\":0,\"message\":\"ok\",\"data\":null}");
    return;
  }

  const int inviteTimeout = mediaSvc.getPreviewInviteTimeoutSec();
  std::string body = "{\"code\":0,\"message\":\"ok\",\"data\":{";
  body += "\"sessionId\":\"" + escapeJsonString(session->sessionId) + "\",";
  body += "\"streamId\":\"" + escapeJsonString(session->streamId) + "\",";
  body += "\"status\":\"" + std::string(getStreamStatusName(session->status)) + "\",";
  body += "\"errorMessage\":\"" + escapeJsonString(session->errorMessage) + "\",";
  body += "\"flvUrl\":\"" + escapeJsonString(session->flvUrl) + "\",";
  body += "\"wsFlvUrl\":\"" + escapeJsonString(session->wsFlvUrl) + "\",";
  body += "\"zlmPort\":" + std::to_string(static_cast<int>(session->zlmPort)) + ",";
  body += "\"previewInviteTimeoutSec\":" + std::to_string(inviteTimeout);
  body += "}}";
  sendJson(body);
}

void HttpSession::handleReplayStart(const std::string& cameraId, const std::string& body) {
  std::string segStr = extractJsonField(body, "segmentId");
  if (segStr.empty() || !isNumericId(segStr)) {
    sendJsonError(400, "缺少有效的 segmentId（replay_segments.id）");
    return;
  }
  std::string platformGbId = resolvePlatformGbIdFromBody(body);
  std::string dbId;
  std::string deviceGb;
  std::string platGbFromRow;
  if (!gb::resolveCameraRowByPathSegment(cameraId, platformGbId, dbId, deviceGb, platGbFromRow)) {
    sendJsonError(404, "摄像头不存在或多平台同号未传 platformGbId");
    return;
  }
  (void)platGbFromRow;
  std::string checkSql = "SELECT p.gb_id, p.online FROM cameras c JOIN device_platforms p ON c.platform_id = p.id "
                         "WHERE c.id = " +
                         dbId + " LIMIT 1";
  std::string checkOut = gb::execPsql(checkSql.c_str());
  if (checkOut.empty()) {
    sendJsonError(404, "摄像头不存在");
    return;
  }
  size_t nl = checkOut.find('\n');
  std::string line = (nl == std::string::npos) ? checkOut : checkOut.substr(0, nl);
  std::vector<std::string> cols = split(line, '|');
  if (cols.size() < 2) {
    sendJsonError(500, "数据库查询错误");
    return;
  }
  std::string platformGbIdFromDb = cols[0];
  bool platformOnline = (cols[1] == "t" || cols[1] == "true" || cols[1] == "1");
  if (!platformOnline) {
    sendJsonError(4001, "设备离线，无法回放");
    return;
  }

  std::string epochSql =
      "SELECT floor(extract(epoch from s.start_time))::bigint, floor(extract(epoch from s.end_time))::bigint "
      "FROM replay_segments s JOIN replay_tasks t ON t.id = s.task_id WHERE s.id = " +
      segStr + " AND t.camera_id = " + dbId + " LIMIT 1";
  std::string epOut = gb::execPsql(epochSql.c_str());
  if (epOut.empty()) {
    sendJsonError(404, "录像片段不存在");
    return;
  }
  size_t p1 = epOut.find('|');
  std::string e0 = (p1 == std::string::npos) ? epOut : epOut.substr(0, p1);
  std::string e1 = (p1 == std::string::npos) ? "" : epOut.substr(p1 + 1);
  nl = e1.find('\n');
  if (nl != std::string::npos) e1 = e1.substr(0, nl);
  uint64_t t0 = static_cast<uint64_t>(std::atoll(e0.c_str()));
  uint64_t t1 = static_cast<uint64_t>(std::atoll(e1.c_str()));
  const uint64_t segLo = std::min(t0, t1);
  const uint64_t segHi = std::max(t0, t1);
  if (segHi <= segLo) {
    sendJsonError(400, "片段时间无效");
    return;
  }

  constexpr uint64_t kMinPlaybackSpanSec = 10;
  std::string offStr = extractJsonField(body, "offsetSeconds");
  std::string psu = extractJsonField(body, "playbackStartUnix");
  std::string peu = extractJsonField(body, "playbackEndUnix");
  const bool hasOff = !offStr.empty();
  const bool hasPsu = !psu.empty();
  const bool hasPeu = !peu.empty();

  if (hasOff && (hasPsu || hasPeu)) {
    sendJsonError(400, "offsetSeconds 与 playbackStartUnix/playbackEndUnix 不可同时使用");
    return;
  }

  uint64_t play0 = segLo;
  uint64_t play1 = segHi;

  if (hasOff) {
    const int64_t off = std::atoll(offStr.c_str());
    if (off < 0) {
      sendJsonError(400, "offsetSeconds 无效");
      return;
    }
    play0 = segLo + static_cast<uint64_t>(off);
    if (play0 >= segHi) {
      sendJsonError(400, "offsetSeconds 超出片段范围");
      return;
    }
    play1 = segHi;
    if (play1 - play0 < kMinPlaybackSpanSec) {
      sendJsonError(400, "片段剩余时长不足，无法从该偏移起播");
      return;
    }
  } else if (hasPsu || hasPeu) {
    if (!hasPsu) {
      sendJsonError(400, "缺少 playbackStartUnix");
      return;
    }
    play0 = static_cast<uint64_t>(std::atoll(psu.c_str()));
    if (hasPeu) {
      play1 = static_cast<uint64_t>(std::atoll(peu.c_str()));
    } else {
      play1 = segHi;
    }
    if (play0 < segLo) play0 = segLo;
    if (play1 > segHi) play1 = segHi;
    if (play1 <= play0) {
      sendJsonError(400, "回放起止时间无效或越界");
      return;
    }
    if (play1 - play0 < kMinPlaybackSpanSec) {
      if (play0 + kMinPlaybackSpanSec <= segHi) {
        play1 = play0 + kMinPlaybackSpanSec;
      } else if (segHi - segLo >= kMinPlaybackSpanSec) {
        play0 = segHi - kMinPlaybackSpanSec;
        play1 = segHi;
      } else {
        sendJsonError(400, "片段时长不足 10 秒，无法按该起止回放");
        return;
      }
    }
  }

  MediaService& mediaSvc = GetMediaService();
  if (!mediaSvc.initialize()) {
    sendJsonError(4003, "媒体服务初始化失败");
    return;
  }

  const std::string streamId =
      MediaService::buildStreamId(platformGbIdFromDb, deviceGb) + "_rp_" + segStr;
  auto session =
      mediaSvc.createSessionWithStreamId(streamId, deviceGb, platformGbIdFromDb, platformGbIdFromDb, dbId);
  if (!session) {
    sendJsonError(500, "创建回放会话失败");
    return;
  }

  bool streamExists = mediaSvc.isStreamExistsInZlm(streamId);
  if (streamExists) {
    mediaSvc.closeRtpServer(streamId);
  }

  std::string sdpConnIp;
  bool previewUseTcp = false;
  resolvePreviewStreamParams(platformGbIdFromDb, sdpConnIp, previewUseTcp);
  const int zlmTcpMode = previewUseTcp ? 1 : 0;
  uint16_t zlmPort = 0;
  if (!mediaSvc.openRtpServer(session->streamId, zlmPort, zlmTcpMode)) {
    mediaSvc.closeSession(session->sessionId);
    sendJsonError(4003, "媒体服务不可用，无法创建收流端口");
    return;
  }
  session->zlmPort = zlmPort;
  session->status = StreamSessionStatus::INVITING;

  std::string callId;
  bool inv = gb::sendPlayInviteAsync(platformGbIdFromDb, deviceGb, zlmPort, callId, sdpConnIp, previewUseTcp, play0,
                                     play1);
  if (!inv) {
    mediaSvc.closeRtpServer(session->streamId);
    mediaSvc.closeSession(session->sessionId);
    sendJsonError(4002, "发送回放 INVITE 失败");
    return;
  }
  session->callId = callId;
  mediaSvc.setSessionCallId(session->sessionId, callId);

  std::string res = "{\"code\":0,\"message\":\"ok\",\"data\":{";
  res += "\"sessionId\":\"" + escapeJsonString(session->sessionId) + "\",";
  res += "\"streamId\":\"" + escapeJsonString(session->streamId) + "\",";
  res += "\"replayStreamId\":\"" + escapeJsonString(session->streamId) + "\",";
  res += "\"status\":\"" + std::string(getStreamStatusName(session->status)) + "\",";
  res += "\"errorMessage\":\"\",";
  res += "\"flvUrl\":\"" + escapeJsonString(session->flvUrl) + "\",";
  res += "\"wsFlvUrl\":\"" + escapeJsonString(session->wsFlvUrl) + "\"";
  res += "}}";
  sendJson(res);
}

void HttpSession::handleReplayStop(const std::string& cameraId, const std::string& body) {
  (void)cameraId;
  std::string streamId = extractJsonField(body, "streamId");
  if (streamId.empty()) {
    sendJsonError(400, "缺少 streamId");
    return;
  }
  MediaService& mediaSvc = GetMediaService();
  auto session = mediaSvc.getSessionByStreamId(streamId);
  if (!session) {
    sendJson("{\"code\":0,\"message\":\"ok\",\"data\":null}");
    return;
  }
  if (!session->callId.empty()) {
    gb::sendPlayByeAsync(session->callId, session->deviceGbId, session->cameraId);
  }
  mediaSvc.closeRtpServer(session->streamId);
  mediaSvc.closeSession(session->sessionId);
  sendJson("{\"code\":0,\"message\":\"ok\",\"data\":null}");
}

/**
 * @brief 处理回放倍速控制请求
 * @details POST /api/cameras/{cameraId}/replay/speed
 *          body: { "streamId": "...", "scale": 4.0 }
 *          通过 SIP INFO + MANSRTSP 向下级设备发送回放速率控制指令。
 *          scale 支持：0.25 / 0.5 / 1 / 2 / 4 / 8 / 16 / 32
 * @param cameraId 摄像头 ID（路径参数）
 * @param body JSON 请求体
 */
void HttpSession::handleReplaySpeed(const std::string& cameraId, const std::string& body) {
  (void)cameraId;
  std::string streamId = extractJsonField(body, "streamId");
  if (streamId.empty()) {
    sendJsonError(400, "缺少 streamId");
    return;
  }
  std::string scaleStr = extractJsonField(body, "scale");
  if (scaleStr.empty()) {
    sendJsonError(400, "缺少 scale（倍速值）");
    return;
  }
  double scale = std::atof(scaleStr.c_str());
  if (scale <= 0 || scale > 32.0) {
    sendJsonError(400, "scale 超出范围（支持 0.25–32）");
    return;
  }

  MediaService& mediaSvc = GetMediaService();
  auto session = mediaSvc.getSessionByStreamId(streamId);
  if (!session) {
    sendJsonError(404, "未找到回放会话");
    return;
  }
  if (session->callId.empty()) {
    sendJsonError(400, "回放会话无 Call-ID，无法发送倍速指令");
    return;
  }

  bool queued = gb::sendPlaybackSpeedInfoAsync(
      session->callId, session->deviceGbId, session->cameraId, scale);
  if (!queued) {
    sendJsonError(500, "倍速指令入队失败");
    return;
  }

  InfoL << "【handleReplaySpeed】Scale=" << scale
        << " queued for stream=" << streamId
        << " callId=" << session->callId;

  sendJson("{\"code\":0,\"message\":\"ok\",\"data\":{\"scale\":" + scaleStr + "}}");
}

void HttpSession::sendReplaySessionStatus(const std::string& cameraId, const std::string& query) {
  (void)cameraId;
  std::string streamId = getQueryParam(query, "streamId");
  if (streamId.empty()) {
    sendJson("{\"code\":0,\"message\":\"ok\",\"data\":null}");
    return;
  }
  MediaService& mediaSvc = GetMediaService();
  auto session = mediaSvc.getSessionByStreamId(streamId);
  if (!session) {
    sendJson("{\"code\":0,\"message\":\"ok\",\"data\":null}");
    return;
  }
  const int inviteTimeout = mediaSvc.getPreviewInviteTimeoutSec();
  std::string res = "{\"code\":0,\"message\":\"ok\",\"data\":{";
  res += "\"sessionId\":\"" + escapeJsonString(session->sessionId) + "\",";
  res += "\"streamId\":\"" + escapeJsonString(session->streamId) + "\",";
  res += "\"status\":\"" + std::string(getStreamStatusName(session->status)) + "\",";
  res += "\"errorMessage\":\"" + escapeJsonString(session->errorMessage) + "\",";
  res += "\"flvUrl\":\"" + escapeJsonString(session->flvUrl) + "\",";
  res += "\"wsFlvUrl\":\"" + escapeJsonString(session->wsFlvUrl) + "\",";
  res += "\"zlmPort\":" + std::to_string(static_cast<int>(session->zlmPort)) + ",";
  res += "\"previewInviteTimeoutSec\":" + std::to_string(inviteTimeout);
  res += "}}";
  sendJson(res);
}

void HttpSession::handleReplayDownloadPost(const std::string& body) {
  ensureReplayDownloadFilePathColumn();
  std::string segStr = extractJsonField(body, "segmentId");
  if (segStr.empty() || !isNumericId(segStr)) {
    sendJsonError(400, "缺少 segmentId");
    return;
  }
  std::string cameraId = extractJsonField(body, "cameraId");
  if (cameraId.empty()) {
    sendJsonError(400, "缺少 cameraId");
    return;
  }
  std::string platformGbId = resolvePlatformGbIdFromBody(body);
  std::string dbId;
  std::string deviceGb;
  std::string platGbFromRow;
  if (!gb::resolveCameraRowByPathSegment(cameraId, platformGbId, dbId, deviceGb, platGbFromRow)) {
    sendJsonError(404, "摄像头不存在或多平台同号未传 platformGbId");
    return;
  }
  std::string checkSql = "SELECT p.gb_id, p.online FROM cameras c JOIN device_platforms p ON c.platform_id=p.id "
                         "WHERE c.id=" +
                         dbId + " LIMIT 1";
  std::string checkOut = gb::execPsql(checkSql.c_str());
  if (checkOut.empty()) {
    sendJsonError(404, "摄像头不存在");
    return;
  }
  size_t nl = checkOut.find('\n');
  std::string line = nl == std::string::npos ? checkOut : checkOut.substr(0, nl);
  std::vector<std::string> cols = split(line, '|');
  if (cols.size() < 2) {
    sendJsonError(500, "数据库错误");
    return;
  }
  std::string platformGbIdFromDb = cols[0];
  bool online = (cols[1] == "t" || cols[1] == "true" || cols[1] == "1");
  if (!online) {
    sendJsonError(4001, "平台离线");
    return;
  }

  std::string verify =
      "SELECT s.id FROM replay_segments s JOIN replay_tasks t ON t.id=s.task_id WHERE s.id=" + segStr +
      " AND t.camera_id=" + dbId + " LIMIT 1";
  if (gb::execPsql(verify.c_str()).empty()) {
    sendJsonError(404, "片段不存在");
    return;
  }

  std::string ins =
      "INSERT INTO replay_downloads (segment_id, status, progress) VALUES (" + segStr +
      ",'processing',0) RETURNING id";
  std::string idOut = gb::execPsql(ins.c_str());
  if (idOut.empty()) {
    sendJsonError(500, "创建下载任务失败");
    return;
  }
  nl = idOut.find('\n');
  if (nl != std::string::npos) idOut = idOut.substr(0, nl);
  int64_t downloadId = std::atoll(idOut.c_str());
  int64_t segPk = std::atoll(segStr.c_str());

  std::thread([downloadId, segPk, deviceGb, dbId, platformGbIdFromDb]() {
    runReplayDownloadJob(downloadId, segPk, deviceGb, dbId, platformGbIdFromDb);
  }).detach();

  std::string res = "{\"code\":0,\"message\":\"ok\",\"data\":{\"downloadId\":" + std::to_string(downloadId) + "}}";
  sendJson(res);
}

void HttpSession::sendReplayDownloadStatus(const std::string& idStr) {
  ensureReplayDownloadFilePathColumn();
  gb::execPsqlCommand("ALTER TABLE replay_downloads ADD COLUMN IF NOT EXISTS download_url VARCHAR(2048)");
  if (idStr.empty() || !isNumericId(idStr)) {
    sendJsonError(400, "无效 id");
    return;
  }
  std::string sql =
      "SELECT status, COALESCE(progress,0), COALESCE(file_path,''), COALESCE(download_url,'') "
      "FROM replay_downloads WHERE id=" + idStr + " LIMIT 1";
  std::string out = gb::execPsql(sql.c_str());
  if (out.empty()) {
    sendJsonError(404, "任务不存在");
    return;
  }
  std::vector<std::string> cols = split(out, '|');
  // 去除尾部换行
  for (auto& c : cols) {
    size_t nl = c.find('\n');
    if (nl != std::string::npos) c = c.substr(0, nl);
  }
  std::string st = cols.size() > 0 ? cols[0] : "";
  std::string pr = cols.size() > 1 ? cols[1] : "0";
  std::string fp = cols.size() > 2 ? cols[2] : "";
  std::string dlUrl = cols.size() > 3 ? cols[3] : "";

  std::string res = "{\"code\":0,\"message\":\"ok\",\"data\":{";
  res += "\"id\":" + idStr + ",";
  res += "\"status\":\"" + escapeJsonString(st) + "\",";
  res += "\"progress\":" + pr + ",";
  res += "\"filePath\":\"" + escapeJsonString(fp) + "\",";
  res += "\"downloadUrl\":\"" + escapeJsonString(dlUrl) + "\"";
  res += "}}";
  sendJson(res);
}

void HttpSession::sendReplayDownloadFile(const std::string& idStr) {
  ensureReplayDownloadFilePathColumn();
  if (idStr.empty() || !isNumericId(idStr)) {
    sendJsonError(400, "无效 id");
    return;
  }
  std::string sql = "SELECT COALESCE(file_path,''), status FROM replay_downloads WHERE id=" + idStr + " LIMIT 1";
  std::string out = gb::execPsql(sql.c_str());
  if (out.empty()) {
    sendNotFound();
    return;
  }
  size_t p1 = out.find('|');
  std::string fp = out.substr(0, p1);
  std::string st = (p1 == std::string::npos) ? "" : out.substr(p1 + 1);
  size_t nl = fp.find('\n');
  if (nl != std::string::npos) fp = fp.substr(0, nl);
  nl = st.find('\n');
  if (nl != std::string::npos) st = st.substr(0, nl);
  if (st != "ready" || fp.empty()) {
    sendJsonError(400, "文件未就绪");
    return;
  }
  // 根据文件扩展名选择下载文件名
  std::string ext = ".mp4";
  if (fp.size() > 4) {
    std::string fileExt = fp.substr(fp.size() - 4);
    if (fileExt == ".flv") ext = ".flv";
  }
  sendFileDownload(fp, "replay_" + idStr + ext);
}

/**
 * @brief 取消进行中的回放下载任务
 * @details POST /api/replay/download/{id}/cancel
 *          将 downloadId 加入全局取消集合，后台下载线程检测到后自动清理资源。
 */
void HttpSession::handleReplayDownloadCancel(const std::string& idStr) {
  if (idStr.empty() || !isNumericId(idStr)) {
    sendJsonError(400, "无效 id");
    return;
  }
  int64_t downloadId = std::atoll(idStr.c_str());

  markDownloadCancelled(downloadId);

  // 如果后台线程已完成录制（status=ready），需要主动清理 ZLM 录制文件
  gb::execPsqlCommand("ALTER TABLE replay_downloads ADD COLUMN IF NOT EXISTS download_url VARCHAR(2048)");
  std::string sql = "SELECT COALESCE(file_path,''), status FROM replay_downloads WHERE id=" + idStr + " LIMIT 1";
  std::string out = gb::execPsql(sql.c_str());
  if (!out.empty()) {
    size_t p1 = out.find('|');
    std::string fp = out.substr(0, p1);
    size_t nl = fp.find('\n');
    if (nl != std::string::npos) fp = fp.substr(0, nl);
    std::string st = (p1 != std::string::npos) ? out.substr(p1 + 1) : "";
    nl = st.find('\n');
    if (nl != std::string::npos) st = st.substr(0, nl);

    if (!fp.empty() && (st == "ready" || st == "processing")) {
      size_t sepPos = fp.find("::");
      std::string dlStreamId = (sepPos != std::string::npos) ? fp.substr(0, sepPos) : fp;
      MediaService& mediaSvc = GetMediaService();
      mediaSvc.deleteRecordDirectory(dlStreamId);
      InfoL << "【handleReplayDownloadCancel】清理 ZLM 录制 stream=" << dlStreamId;
    }
  }

  gb::execPsqlCommand("UPDATE replay_downloads SET status='cancelled',file_path='',download_url='',updated_at=CURRENT_TIMESTAMP WHERE id=" +
                      idStr + " AND (status='processing' OR status='ready')");

  InfoL << "【handleReplayDownloadCancel】downloadId=" << downloadId << " 已标记取消";
  sendJson("{\"code\":0,\"message\":\"ok\",\"data\":null}");
}

/**
 * @brief 浏览器下载完成后清理 ZLM 录制文件
 * @details POST /api/replay/download/{id}/cleanup
 *          通过 ZLM deleteRecordDirectory API 删除录制文件，不依赖磁盘路径。
 *          file_path 列存储 "streamId|dateStr" 格式，解析后调 ZLM API 删除。
 */
void HttpSession::handleReplayDownloadCleanup(const std::string& idStr) {
  if (idStr.empty() || !isNumericId(idStr)) {
    sendJsonError(400, "无效 id");
    return;
  }
  gb::execPsqlCommand("ALTER TABLE replay_downloads ADD COLUMN IF NOT EXISTS download_url VARCHAR(2048)");
  std::string sql = "SELECT COALESCE(file_path,''), status FROM replay_downloads WHERE id=" + idStr + " LIMIT 1";
  std::string out = gb::execPsql(sql.c_str());
  if (out.empty()) {
    sendJson("{\"code\":0,\"message\":\"ok\",\"data\":null}");
    return;
  }
  size_t p1 = out.find('|');
  std::string fp = out.substr(0, p1);
  size_t nl = fp.find('\n');
  if (nl != std::string::npos) fp = fp.substr(0, nl);

  // file_path 格式: "streamId::dateStr"，解析后调 ZLM API 删除
  if (!fp.empty()) {
    size_t sepPos = fp.find("::");
    std::string dlStreamId = (sepPos != std::string::npos) ? fp.substr(0, sepPos) : fp;
    std::string dlDateStr = (sepPos != std::string::npos) ? fp.substr(sepPos + 2) : "";

    MediaService& mediaSvc = GetMediaService();
    if (mediaSvc.deleteRecordDirectory(dlStreamId, dlDateStr)) {
      InfoL << "【DownloadCleanup】ZLM 录制目录已删除 stream=" << dlStreamId << " date=" << dlDateStr;
    } else {
      InfoL << "【DownloadCleanup】ZLM 删除返回非0（目录可能已不存在） stream=" << dlStreamId;
    }
    // 同时尝试删除整个流的录制目录（如果日期目录下只有这一天）
    mediaSvc.deleteRecordDirectory(dlStreamId);
  }

  gb::execPsqlCommand("UPDATE replay_downloads SET status='cleaned',file_path='',download_url='',updated_at=CURRENT_TIMESTAMP WHERE id=" + idStr);
  sendJson("{\"code\":0,\"message\":\"ok\",\"data\":null}");
}

/**
 * @brief 处理 POST /api/ptz：校验摄像机与平台在线后入队云台 DeviceControl
 * @param body JSON：cameraId、command、action、speed(可选)、platformGbId/platformId(可选，用于多平台歧义消解)
 * @details 从 device_platforms 取平台在线状态与 gb_id，调用 gb::enqueuePtzDeviceControl。
 *          成功返回 code 0；业务错误使用 sendJsonError（400/404/4001 等）。
 * @note 不阻塞等待 SIP 对端响应；仅表示已入队并由 PJSIP 线程发送 MESSAGE
 */
void HttpSession::handlePtzControl(const std::string& body) {
  std::string cameraId = extractJsonField(body, "cameraId");
  std::string command = extractJsonField(body, "command");
  std::string action = extractJsonField(body, "action");
  std::string speedStr = extractJsonField(body, "speed");
  int speed = 2;
  if (!speedStr.empty()) speed = std::atoi(speedStr.c_str());
  if (speed < 1) speed = 1;
  if (speed > 3) speed = 3;

  if (cameraId.empty() || command.empty() || action.empty()) {
    sendJsonError(400, "cameraId、command、action 不能为空");
    return;
  }

  std::string platformGbId = resolvePlatformGbIdFromBody(body);
  std::string dbId;
  std::string deviceGb;
  std::string platGbFromRow;
  if (!gb::resolveCameraRowByPathSegment(cameraId, platformGbId, dbId, deviceGb, platGbFromRow)) {
    sendJsonError(404, "摄像头不存在或多平台同号未传 platformGbId");
    return;
  }
  (void)platGbFromRow;

  std::string checkSql = "SELECT p.gb_id, p.online FROM cameras c JOIN device_platforms p ON c.platform_id = p.id "
                         "WHERE c.id = " +
                         dbId + " LIMIT 1";
  std::string checkOut = gb::execPsql(checkSql.c_str());
  if (checkOut.empty()) {
    sendJsonError(404, "摄像头不存在");
    return;
  }

  size_t nl = checkOut.find('\n');
  std::string line = (nl == std::string::npos) ? checkOut : checkOut.substr(0, nl);
  std::vector<std::string> cols;
  std::string cur;
  for (char c : line) {
    if (c == '|') {
      cols.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  cols.push_back(cur);
  if (cols.size() < 2) {
    sendJsonError(500, "数据库查询错误");
    return;
  }

  std::string platformGbIdFromDb = cols[0];
  bool platformOnline = (cols[1] == "t" || cols[1] == "true" || cols[1] == "1");
  if (!platformOnline) {
    sendJsonError(4001, "设备离线，无法云台控制");
    return;
  }

  if (!gb::enqueuePtzDeviceControl(platformGbIdFromDb, deviceGb, command, action, speed)) {
    sendJsonError(400, "云台指令无效或 SIP 未就绪");
    return;
  }
  sendJson("{\"code\":0,\"message\":\"ok\",\"data\":null}");
}

// ZLM Hook事件处理 - 无人观看
void HttpSession::handleZlmHookNoneReader(const std::string& body) {
  InfoL << "【handleZlmHookNoneReader】ZLM on_stream_none_reader body: " << body;

  /* 候选 stream 标识：ZLM 文档为 stream；部分版本/协议可能带 stream_id，或与 app 组合成 app/stream */
  std::vector<std::string> candidates;
  auto addCand = [&](const std::string& s) {
    if (s.empty()) {
      return;
    }
    if (std::find(candidates.begin(), candidates.end(), s) == candidates.end()) {
      candidates.push_back(s);
    }
  };
  addCand(extractJsonField(body, "stream"));
  addCand(extractJsonField(body, "stream_id"));
  const std::string app = extractJsonField(body, "app");
  const std::string streamOnly = extractJsonField(body, "stream");
  if (!app.empty() && !streamOnly.empty()) {
    addCand(app + "/" + streamOnly);
  }

  MediaService& mediaSvc = GetMediaService();
  bool matched = false;
  for (const auto& sid : candidates) {
    if (mediaSvc.handleStreamNoneReader(sid)) {
      matched = true;
      InfoL << "【handleZlmHookNoneReader】handled none_reader matched streamId=" << sid;
      break;
    }
  }
  if (!matched) {
    WarnL << "【handleZlmHookNoneReader】no gb_service session matched any candidate (count="
          << candidates.size() << "); ZLM 仍应释放本路媒体，见 close=true";
  }

  /* 必须 close=true，否则 ZLM 不拆流，设备 RTP 可能一直进 ZLM。无本地会话时同样返回 true 以清理 orphan。 */
  sendJson("{\"code\":0,\"close\":true}");
}

// ZLM Hook事件处理 - 流状态变更（注册/注销）
void HttpSession::handleZlmHookStreamChanged(const std::string& body) {
  const std::string registValue = extractJsonField(body, "regist");
  const bool isRegist = (registValue == "true" || registValue == "1");

  /* 与 handleZlmHookNoneReader 一致：ZLM 的 stream 字段未必等于 gb 内 openRtpServer 的 stream_id */
  std::vector<std::string> candidates;
  auto addCand = [&](const std::string& s) {
    if (s.empty()) {
      return;
    }
    if (std::find(candidates.begin(), candidates.end(), s) == candidates.end()) {
      candidates.push_back(s);
    }
  };
  addCand(extractJsonField(body, "stream"));
  addCand(extractJsonField(body, "stream_id"));
  const std::string app = extractJsonField(body, "app");
  const std::string streamOnly = extractJsonField(body, "stream");
  if (!app.empty() && !streamOnly.empty()) {
    addCand(app + "/" + streamOnly);
  }

  InfoL << "[ZLM hook] t_ms=" << zlmHookSteadyMs() << " hook=on_stream_changed candidates=" << candidates.size()
        << " regist=" << (isRegist ? "true" : "false");
  InfoL << "【handleZlmHookStreamChanged】body: " << body;

  MediaService& mediaSvc = GetMediaService();
  bool matched = false;
  for (const auto& sid : candidates) {
    if (mediaSvc.handleStreamChanged(sid, isRegist)) {
      matched = true;
      InfoL << "【handleZlmHookStreamChanged】matched streamId=" << sid
            << " regist=" << (isRegist ? "true" : "false");
      break;
    }
  }
  if (!matched) {
    WarnL << "【handleZlmHookStreamChanged】no session matched any candidate (BYE/状态更新均未执行) count="
          << candidates.size() << " regist=" << (isRegist ? "true" : "false");
  }

  sendJson("{\"code\":0}");
}

// ZLM Hook：openRtpServer 长期无 RTP（对回复不敏感）
void HttpSession::handleZlmHookRtpServerTimeout(const std::string& body) {
  std::string streamId = extractJsonField(body, "stream_id");
  if (streamId.empty()) {
    streamId = extractJsonField(body, "stream");
  }
  const std::string localPort = extractJsonField(body, "local_port");
  const std::string tcpMode = extractJsonField(body, "tcp_mode");

  InfoL << "[ZLM hook] t_ms=" << zlmHookSteadyMs() << " hook=on_rtp_server_timeout stream_id=" << streamId
        << " local_port=" << localPort << " tcp_mode=" << tcpMode;
  InfoL << "【handleZlmHookRtpServerTimeout】body: " << body;

  MediaService& mediaSvc = GetMediaService();
  mediaSvc.handleRtpServerTimeout(streamId);
  sendJson("{\"code\":0}");
}

// ZLM Hook：startSendRtp 推流停止（对端断流或链路异常）
void HttpSession::handleZlmHookSendRtpStopped(const std::string& body) {
  std::string streamId = extractJsonField(body, "stream_id");
  if (streamId.empty()) {
    streamId = extractJsonField(body, "stream");
  }
  const std::string ssrc = extractJsonField(body, "ssrc");

  InfoL << "[ZLM hook] t_ms=" << zlmHookSteadyMs() << " hook=on_send_rtp_stopped stream_id=" << streamId
        << " ssrc=" << ssrc;
  InfoL << "【handleZlmHookSendRtpStopped】body: " << body;

  // 调用 UpstreamInviteBridge 处理上级断流
  gb::upstreamBridgeOnZlmSendRtpStopped(streamId, ssrc);
  sendJson("{\"code\":0}");
}

void HttpSession::sendNotFound() {
  const char body[] = "Not Found";
  std::string response;
  response += "HTTP/1.1 404 Not Found\r\n";
  response += "Content-Type: text/plain; charset=utf-8\r\n";
  response += "Content-Length: ";
  response += std::to_string(sizeof(body) - 1);
  response += "\r\nConnection: close\r\n\r\n";
  response += body;

  auto buf = toolkit::BufferRaw::create(response.size());
  buf->assign(response.data(), response.size());
  send(buf);
}

/**
 * @brief 接收数据回调
 * @param buf 接收到的数据缓冲区
 */
void HttpSession::onRecv(const toolkit::Buffer::Ptr& buf) {
  recvBuffer_.append(buf->data(), buf->size());
  InfoL << "HTTP request from " << get_peer_ip() << ", buffered=" << recvBuffer_.size();

  std::string request;
  while (tryExtractCompleteHttpRequest(recvBuffer_, request)) {
    handleRequest(request.data(), request.size());
    request.clear();
  }
}

/**
 * @brief 错误回调
 * @param err Socket 异常
 */
void HttpSession::onError(const toolkit::SockException& err) {
  WarnL << "HttpSession " << getIdentifier() << " error: " << err;
}

/**
 * @brief 定时管理回调
 */
void HttpSession::onManager() {
  // 骨架阶段无超时管理，留空即可
}

}  // namespace gb
