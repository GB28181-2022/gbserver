/**
 * @file SipServerPjsip.h
 * @brief GB28181 SIP 服务端模块（基于 PJSIP）
 * @details 实现 GB/T 28181-2016 协议的 SIP 信令服务：
 *          - 基于 PJSIP 协议栈实现
 *          - 支持 UDP/TCP 双栈传输
 *          - 处理 REGISTER、MESSAGE、NOTIFY 请求
 *          - 从 gb_local_config 读取信令端口和传输配置
 *          - 异步 Catalog 查询队列处理
 * @date 2025
 * @note 依赖 PJSIP 第三方库，支持海康、大华等设备接入
 */
#ifndef GB_SERVICE_SIP_SERVER_PJSIP_H
#define GB_SERVICE_SIP_SERVER_PJSIP_H

#include <string>

// 前向声明 pjsip_rx_data
struct pjsip_rx_data;

namespace gb {

/**
 * @struct SipServerConfig
 * @brief SIP 服务配置结构体
 * @details 存储 SIP 服务启动参数，从 gb_local_config 表读取
 */
struct SipServerConfig {
  int signal_port = 5060;       /**< 信令端口，默认 5060（GB28181 标准端口） */
  bool transport_udp = true;    /**< 是否启用 UDP 传输 */
  bool transport_tcp = false;   /**< 是否启用 TCP 传输 */
};

/**
 * @brief 从数据库加载 SIP 服务配置
 * @return 解析后的 SIP 服务配置
 * @details 查询 gb_local_config 表的 signal_port、transport_udp、transport_tcp 字段
 */
SipServerConfig loadSipServerConfig();

/**
 * @brief 启动 PJSIP SIP 服务
 * @param cfg SIP 服务配置
 * @return true 表示启动成功，false 表示失败
 * @details 初始化流程：
 *          1. 初始化 PJLIB 库
 *          2. 创建 PJSIP 端点
 *          3. 根据配置启动 UDP/TCP 传输
 *          4. 注册 SIP 模块
 *          5. 启动工作线程处理 SIP 事件
 * @note 必须在主线程调用，服务启动后才能处理 SIP 消息
 */
bool SipServerPjsipStart(const SipServerConfig& cfg);

/**
 * @brief 停止 PJSIP 服务
 * @details 关闭流程：
 *          1. 停止工作线程
 *          2. 销毁 PJSIP 端点
 *          3. 释放 PJLIB 资源
 * @note 优雅关闭，确保所有资源正确释放
 */
void SipServerPjsipStop();

/**
 * @brief 刷新系统配置缓存
 * @details 当系统配置（如 SIP 域、国标 ID、密码等）被修改后调用，
 *          重新从数据库加载配置到内存缓存，避免重启服务
 * @note 线程安全，可在运行时调用
 */
void reloadSystemConfigCache();

/**
 * @brief 获取系统配置缓存的 realm（SIP 域）
 * @return SIP 域字符串（高性能，内存读取）
 * @note 如果缓存未加载，会自动从数据库加载
 */
const char* getSystemRealm();

/**
 * @brief 获取系统配置缓存的密码
 * @return 鉴权密码字符串（高性能，内存读取）
 * @note 如果缓存未加载，会自动从数据库加载
 */
const char* getSystemPassword();

/**
 * @brief 获取系统配置缓存的本机国标 ID
 * @return 本机国标 ID 字符串（高性能，内存读取）
 * @note 如果缓存未加载，会自动从数据库加载
 */
const char* getSystemGbId();

/**
 * @brief 注册成功后是否自动向该下级发 Catalog 查询（来自 gb_local_config）
 */
bool isCatalogOnRegisterEnabled();

/**
 * @brief 注册触发 Catalog 的按平台冷却时间（秒）；0 表示不启用冷却
 */
int getCatalogOnRegisterCooldownSec();

/**
 * @brief 从入站报文取信令源地址（NAT 出口）
 * @param rdata 收到的 SIP 消息
 * @param outIp 输出信令源 IP
 * @param outPort 输出信令源端口
 * @note 优先 pkt_info.src_name/src_port；部分场景仅填充 src_addr，需回退打印
 */
void extractSignalSrcFromRdata(pjsip_rx_data* rdata, std::string& outIp, int& outPort);

}  // namespace gb

#endif
