/**
 * @file SipDigest.cpp
 * @brief SIP Digest 鉴权实现
 * @details 实现 GB/T 28181-2016 附录 F 数字摘要认证：
 *          - Authorization 头解析
 *          - MD5 摘要计算（使用 ZLToolKit MD5 实现）
 *          - Digest 响应校验
 *          算法流程：
 *          1. HA1 = MD5(username:realm:password)
 *          2. HA2 = MD5(method:uri)
 *          3. response = MD5(HA1:nonce:HA2)
 * @date 2025
 * @note 仅支持 MD5 算法
 */
#include "infra/SipDigest.h"
#include "Util/MD5.h"
#include <algorithm>
#include <cctype>
#include <sstream>

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
 * @brief 从 Authorization 头解析参数值
 * @param line Authorization 头内容
 * @param name 参数名（如 username、realm 等）
 * @param out  输出参数值
 * @return true 解析成功，false 解析失败
 * @details 支持两种格式：
 *          - name="value"（带引号）
 *          - name=value（无引号）
 */
bool parseParam(const std::string& line, const std::string& name, std::string& out) {
  std::string key = name + "=";
  size_t i = line.find(key);
  if (i == std::string::npos) return false;
  i += key.size();
  if (i >= line.size()) return false;
  if (line[i] == '"') {
    ++i;
    size_t end = line.find('"', i);
    if (end == std::string::npos) return false;
    out = line.substr(i, end - i);
    return true;
  }
  size_t end = i;
  while (end < line.size() && line[end] != ',' && !std::isspace(static_cast<unsigned char>(line[end]))) ++end;
  out = trim(line.substr(i, end - i));
  return true;
}

/**
 * @brief 计算 MD5 摘要的十六进制字符串
 * @param in 输入字符串
 * @return MD5 摘要的十六进制表示（32位小写）
 */
std::string md5Hex(const std::string& in) {
  return toolkit::MD5(in).hexdigest();
}

}  // namespace

/**
 * @brief 解析 Authorization: Digest 头
 * @param authHeader  Authorization 头完整内容
 * @param username    输出：用户名
 * @param realm       输出：域
 * @param nonce       输出：随机数
 * @param uri         输出：请求 URI
 * @param response    输出：响应摘要
 * @param algorithm   可选输出：算法
 * @param opaque      可选输出：不透明参数
 * @return true 解析成功
 * @details 解析流程：
 *          1. 检查头部是否以 "Digest" 开头（不区分大小写）
 *          2. 提取各个参数值
 *          3. 必填参数：username、realm、nonce、uri、response
 *          4. 可选参数：algorithm、opaque
 */
bool parseDigestAuth(const std::string& authHeader,
                    std::string& username,
                    std::string& realm,
                    std::string& nonce,
                    std::string& uri,
                    std::string& response,
                    std::string* algorithm,
                    std::string* opaque) {
  username.clear();
  realm.clear();
  nonce.clear();
  uri.clear();
  response.clear();
  if (algorithm) algorithm->clear();
  if (opaque) opaque->clear();
  std::string h = trim(authHeader);
  if (h.size() < 7) return false;
  if (std::tolower(static_cast<unsigned char>(h[0])) != 'd' ||
      std::tolower(static_cast<unsigned char>(h[1])) != 'i' ||
      std::tolower(static_cast<unsigned char>(h[2])) != 'g' ||
      std::tolower(static_cast<unsigned char>(h[3])) != 'e' ||
      std::tolower(static_cast<unsigned char>(h[4])) != 's' ||
      std::tolower(static_cast<unsigned char>(h[5])) != 't' ||
      (h[6] != ' ' && h[6] != '\t')) return false;
  size_t start = 7;
  while (start < h.size() && (h[start] == ' ' || h[start] == '\t')) ++start;
  std::string rest = h.substr(start);
  if (!parseParam(rest, "username", username)) return false;
  if (!parseParam(rest, "realm", realm)) return false;
  if (!parseParam(rest, "nonce", nonce)) return false;
  if (!parseParam(rest, "uri", uri)) return false;
  if (!parseParam(rest, "response", response)) return false;
  if (algorithm) parseParam(rest, "algorithm", *algorithm);
  if (opaque) parseParam(rest, "opaque", *opaque);
  return true;
}

/**
 * @brief 不区分大小写的字符串比较
 * @param a 字符串 a
 * @param b 字符串 b
 * @return true 相等，false 不相等
 */
static bool strCaseEqual(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  return true;
}

/**
 * @brief 校验客户端 Digest 响应
 * @param username        用户名
 * @param realm           域
 * @param password        密码
 * @param method          SIP 方法
 * @param uri             请求 URI
 * @param nonce           随机数
 * @param clientResponse  客户端响应摘要
 * @param algorithm       算法（可选）
 * @return true 校验通过
 * @details 校验流程：
 *          1. 检查必填参数
 *          2. 若指定 algorithm，必须为 MD5
 *          3. 计算 HA1 = MD5(username:realm:password)
 *          4. 计算 HA2 = MD5(method:uri)
 *          5. 计算 expected = MD5(HA1:nonce:HA2)
 *          6. 比较 expected 与 clientResponse
 * @note 符合 GB28181-2016 附录 F 要求
 */
bool verifyDigestResponse(const std::string& username,
                          const std::string& realm,
                          const std::string& password,
                          const std::string& method,
                          const std::string& uri,
                          const std::string& nonce,
                          const std::string& clientResponse,
                          const std::string& algorithm) {
  if (username.empty() || realm.empty() || nonce.empty() || uri.empty() || clientResponse.empty())
    return false;
  /* GB28181：仅接受 algorithm=MD5；若客户端带 algorithm 则必须为 MD5 */
  if (!algorithm.empty() && !strCaseEqual(algorithm, "MD5"))
    return false;
  std::string ha1 = md5Hex(username + ":" + realm + ":" + password);
  std::string ha2 = md5Hex(method + ":" + uri);
  std::string expected = md5Hex(ha1 + ":" + nonce + ":" + ha2);
  return expected == clientResponse;
}

}  // namespace gb
