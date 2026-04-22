/**
 * @file SipDigest.h
 * @brief SIP Digest 鉴权模块
 * @details 实现 GB/T 28181-2016 附录 F 数字摘要认证：
 *          - 解析 Authorization: Digest 头
 *          - MD5 摘要算法计算和校验
 *          - 与 RFC 2617 兼容
 *          算法：HA1=MD5(username:realm:password)
 *                HA2=MD5(method:uri)
 *                response=MD5(HA1:nonce:HA2)
 * @date 2025
 * @note 仅支持 MD5 算法（GB28181 标准要求）
 */
#ifndef GB_SERVICE_SIP_DIGEST_H
#define GB_SERVICE_SIP_DIGEST_H

#include <string>

namespace gb {

/**
 * @brief 解析 Authorization: Digest 头
 * @param authHeader  Authorization 头完整内容
 * @param username    输出：用户名
 * @param realm       输出：域
 * @param nonce       输出：随机数
 * @param uri         输出：请求 URI
 * @param response    输出：响应摘要
 * @param algorithm   可选输出：算法（如 MD5）
 * @param opaque      可选输出：不透明参数
 * @return true 解析成功，false 解析失败
 * @details 解析格式：Digest username="...", realm="...", nonce="...",
 *          uri="...", response="...", algorithm=MD5
 */
bool parseDigestAuth(const std::string& authHeader,
                     std::string& username,
                     std::string& realm,
                     std::string& nonce,
                     std::string& uri,
                     std::string& response,
                     std::string* algorithm = nullptr,
                     std::string* opaque = nullptr);

/**
 * @brief 校验客户端 Digest 响应
 * @param username      用户名
 * @param realm         域
 * @param password      密码
 * @param method        SIP 方法（如 REGISTER）
 * @param uri           请求 URI
 * @param nonce         随机数
 * @param clientResponse 客户端发送的响应摘要
 * @param algorithm     算法（仅支持 MD5，为空则不检查）
 * @return true 校验通过，false 校验失败
 * @details 按 GB28181-2016 附录 F 计算预期响应并与客户端响应比对
 * @note 若客户端指定 algorithm 则必须为 MD5，否则拒绝
 */
bool verifyDigestResponse(const std::string& username,
                          const std::string& realm,
                          const std::string& password,
                          const std::string& method,
                          const std::string& uri,
                          const std::string& nonce,
                          const std::string& clientResponse,
                          const std::string& algorithm = "");

}  // namespace gb

#endif
