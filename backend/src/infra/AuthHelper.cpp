/**
 * @file AuthHelper.cpp
 * @brief 认证辅助实现
 * @details 实现用户认证相关功能：
 *          - 密码哈希：使用 SHA256 + 盐值
 *          - Token 生成：使用 /dev/urandom 或随机数生成器
 *          - SQL 转义：单引号转双单引号
 * @date 2025
 * @note 密码哈希依赖 openssl 命令行工具
 */
#include "infra/AuthHelper.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <unistd.h>

namespace gb {

namespace {

const char kSalt[] = "gb_svc_2022";  /**< 密码哈希盐值 */

}  // namespace

/**
 * @brief 计算密码哈希
 * @param password 明文密码
 * @return SHA256 哈希的十六进制字符串
 * @details 实现流程：
 *          1. 盐值 + 密码拼接
 *          2. 写入临时文件
 *          3. 使用 openssl dgst -sha256 计算哈希
 *          4. 使用 xxd -p 转换为十六进制
 *          5. 清理临时文件
 */
std::string hashPasswordDefault(const std::string& password) {
  std::string input = std::string(kSalt) + password;
  char tmp[] = "/tmp/gb_auth_XXXXXX";
  int fd = mkstemp(tmp);
  if (fd < 0) return {};
  FILE* fp = fdopen(fd, "w");
  if (!fp) {
    close(fd);
    unlink(tmp);
    return {};
  }
  size_t n = fwrite(input.data(), 1, input.size(), fp);
  fclose(fp);
  if (n != input.size()) {
    unlink(tmp);
    return {};
  }

  std::string cmd = "openssl dgst -sha256 -binary < ";
  cmd += tmp;
  cmd += " 2>/dev/null | xxd -p -c 0 2>/dev/null";
  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe) {
    unlink(tmp);
    return {};
  }
  char buf[128];
  std::string hex;
  while (fgets(buf, sizeof(buf), pipe)) {
    buf[strcspn(buf, "\r\n")] = '\0';
    hex += buf;
  }
  pclose(pipe);
  unlink(tmp);
  while (!hex.empty() && (hex.back() == '\n' || hex.back() == '\r' || hex.back() == ' ')) hex.pop_back();
  return hex;
}

/**
 * @brief 生成随机 Token
 * @return 64 字符十六进制字符串
 * @details 生成流程：
 *          1. 优先使用 /dev/urandom 获取高质量随机数
 *          2. 若失败则使用 std::random_device 回退
 *          3. 转换为十六进制字符串
 */
std::string generateToken() {
  unsigned char bytes[32];
  FILE* fp = fopen("/dev/urandom", "rb");
  if (!fp) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);
    for (int i = 0; i < 32; ++i) bytes[i] = static_cast<unsigned char>(dis(gen));
  } else {
    if (fread(bytes, 1, 32, fp) != 32) {
      fclose(fp);
      return {};
    }
    fclose(fp);
  }
  static const char hex[] = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (int i = 0; i < 32; ++i) {
    out += hex[(bytes[i] >> 4) & 0xf];
    out += hex[bytes[i] & 0xf];
  }
  return out;
}

/**
 * @brief SQL 字符串转义
 * @param s 原始字符串
 * @return 转义后的字符串
 * @details 将单引号 ' 替换为双单引号 ''
 *          这是 PostgreSQL 标准的字符串转义方式
 * @note 必须在拼接 SQL 字符串前调用，防止 SQL 注入
 */
std::string escapeSqlString(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    if (c == '\'') out += "''";
    else out += c;
  }
  return out;
}

}  // namespace gb
