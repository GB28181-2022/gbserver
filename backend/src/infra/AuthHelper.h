/**
 * @file AuthHelper.h
 * @brief 认证辅助模块
 * @details 提供用户认证相关工具函数：
 *          - 密码哈希（SHA256）
 *          - Token 生成
 *          - SQL 字符串转义（防注入）
 * @date 2025
 * @note 依赖系统 openssl 命令行工具进行哈希计算
 */
#ifndef GB_SERVICE_AUTH_HELPER_H
#define GB_SERVICE_AUTH_HELPER_H

#include <string>

namespace gb {

/**
 * @brief 计算密码哈希
 * @param password 明文密码
 * @return SHA256 哈希的十六进制字符串
 * @details 使用系统 openssl 命令计算 SHA256 哈希
 * @note 哈希值用于数据库存储和登录验证
 */
std::string hashPasswordDefault(const std::string& password);

/**
 * @brief 生成随机 Token
 * @return 32 字节随机数的十六进制字符串（64 位字符）
 * @details 使用 /dev/urandom 生成高质量随机数
 * @note 用于用户登录后的身份认证
 */
std::string generateToken();

/**
 * @brief SQL 字符串转义
 * @param s 原始字符串
 * @return 转义后的字符串（将 ' 替换为 ''）
 * @details 防止 SQL 注入攻击，拼接 SQL 前必须转义
 */
std::string escapeSqlString(const std::string& s);

}  // namespace gb

#endif
