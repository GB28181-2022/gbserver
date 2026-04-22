/**
 * @file SipHandler.h
 * @brief 国标 SIP 业务逻辑处理模块
 * @details 实现 GB/T 28181-2016 协议核心功能：
 *          - 设备 REGISTER 注册鉴权与设备信息入库
 *          - MESSAGE/Keepalive 心跳消息处理
 *          - 黑白名单策略管理（whitelist/blacklist/normal）
 *          - 独立配置优先的鉴权逻辑（系统配置 vs 设备独立配置）
 *          - 心跳超时检测与级联状态更新
 *          供 SipServerPjsip 模块调用，处理 SIP 业务逻辑
 * @date 2025
 * @note 与 GB28181-2016 第7章 注册流程、附录F 数字摘要认证一致
 */
#ifndef GB_SERVICE_SIP_HANDLER_H
#define GB_SERVICE_SIP_HANDLER_H

#include <string>

namespace gb {

/**
 * @brief 从 From 头或 URI 字符串解析设备 ID
 * @param fromHeader From 头字段值或 URI 字符串，格式：<sip:DeviceID@host> 或 sip:DeviceID@host
 * @return 解析出的设备 ID，解析失败返回空字符串
 * @details 按 GB28181 规范提取 SIP URI 中的用户部分（DeviceID）
 *          支持带尖括号和不带尖括号两种格式
 */
std::string parseDeviceIdFromFrom(const std::string& fromHeader);

/**
 * @brief 从 XML body 中提取 DeviceID
 * @param body XML 消息体内容
 * @return 提取的 DeviceID 值，提取失败返回空字符串
 * @details 解析 <DeviceID>...</DeviceID> 标签内容
 *          常用于 Keepalive、Catalog 等消息体
 */
std::string parseDeviceIdFromBody(const std::string& body);

/**
 * @brief 判断消息体是否为心跳消息
 * @param body XML 消息体内容
 * @return true 表示是心跳消息，false 表示不是
 * @details 检查消息体是否包含 <CmdType>Keepalive</CmdType> 标记
 *          符合 GB28181-2016 第9.2节 心跳消息格式
 */
bool isKeepaliveBody(const std::string& body);

/**
 * @brief 处理 REGISTER 注册请求（GB28181-2016 第7章）
 * @param deviceId    从 From 头解析出的设备 ID
 * @param expires     Expires 头值（秒），0 表示注销请求
 * @param authHeader  Authorization 头整行（可选），Digest 鉴权时使用
 * @param method      SIP 方法（如 "REGISTER"），用于鉴权校验
 * @param requestUri  请求 URI，用于鉴权校验（应与 Authorization 中的 uri 一致）
 * @param contactIp   Contact 头中的 IP 地址（可选），设备联系地址
 * @param contactPort Contact 头中的端口（可选），设备联系端口
 * @return HTTP 状态码：200 允许，401 需鉴权或鉴权失败，403 拒绝（黑名单）
 * @details 实现完整的注册处理流程：
 *          1. 黑白名单检查（blacklist 直接拒绝，whitelist 直接通过）
 *          2. 鉴权逻辑：独立配置优先（strategy_mode=custom 使用 custom_auth_password）
 *          3. 新设备自动注册（inherit 模式），保存 Contact 信息
 *          4. 更新设备在线状态（级联更新摄像头状态）
 *          5. 保存设备 Contact 地址（IP+端口）到数据库
 *          6. 新平台或从离线到在线时，按 gb_local_config 异步下发 Catalog Query
 * @note 符合 GB28181-2016 第7章注册流程和附录F数字摘要认证
 */
int handleRegister(const std::string& deviceId,
                   int expires,
                   const std::string& authHeader = "",
                   const std::string& method = "REGISTER",
                   const std::string& requestUri = "",
                   const std::string& contactIp = "",
                   int contactPort = 0,
                   const std::string& signalSrcIp = "",
                   int signalSrcPort = 0);

/**
 * @brief 检查心跳消息是否被允许
 * @param deviceId 设备 ID
 * @return HTTP 状态码：200 允许，401 需重新鉴权，403 拒绝（黑名单或不存在）
 * @details 检查逻辑：
 *          - 黑名单或不存在：返回 403
 *          - 白名单：返回 200
 *          - 普通名单且配置了鉴权密码但当前不在线：返回 401（需重新 REGISTER）
 *          - 其他情况：返回 200
 * @note 仅做权限检查，不更新在线状态，状态更新由 handleKeepalive 完成
 */
int checkKeepaliveAuth(const std::string& deviceId);

/**
 * @brief 处理心跳（Keepalive）消息
 * @param deviceId 设备 ID（从 body 或 From 头解析）
 * @param signalSrcIp 报文 UDP 源 IP（pkt_info.src_name），用于 NAT 出口落库
 * @param signalSrcPort 报文 UDP 源端口
 * @details 内存始终更新在线态；数据库按节流写入 signal_src_* 与 last_heartbeat_at（非每包 UPDATE），
 *          映射变化或离线→在线时立即刷新。
 */
void handleKeepalive(const std::string& deviceId,
                      const std::string& signalSrcIp = "",
                      int signalSrcPort = 0);

/**
 * @brief 启动心跳超时检查线程
 * @param timeoutSeconds 超时时间（秒），默认 120 秒
 * @details 后台线程定期检查超过 timeoutSeconds 未更新的设备，
 *          自动标记为离线，并级联更新其下所有摄像头状态
 * @note 检查间隔为 timeoutSeconds/2，确保及时发现超时
 */
void startHeartbeatTimeoutChecker(int timeoutSeconds = 120);

/**
 * @brief 停止心跳超时检查线程
 * @details 优雅关闭超时检查线程，等待线程结束
 */
void stopHeartbeatTimeoutChecker();

}  // namespace gb

#endif
