/**
 * @file main.cpp
 * @brief 国标服务器系统 - 程序入口模块
 * @details 启动 HTTP 8080 服务和 SIP 服务（从 gb_local_config 读取信令端口和传输配置）
 *          - 使用 ZLToolKit 实现 HTTP 会话管理（Reactor 网络模型）
 *          - 使用 PJSIP 实现 SIP 协议栈，支持 UDP/TCP 双栈传输
 *          - 心跳超时置离线，级联更新摄像头状态
 *          - 支持优雅关闭（SIGINT 信号处理）
 * @date 2025
 * @note 依赖 ZLToolKit 和 PJSIP 第三方库
 */

#ifndef _WIN32
#include <signal.h>
#endif
#include <atomic>

#include "Util/logger.h"
#include "Network/TcpServer.h"
#include "Thread/semaphore.h"
#include "infra/HttpSession.h"
#include "infra/DbUtil.h"
#include "infra/MediaService.h"
#include "infra/SipServerPjsip.h"
#include "infra/SipHandler.h"

using namespace toolkit;

static std::atomic<bool> g_running{true};

/**
 * @brief 程序入口函数
 * @return 0 表示正常退出，1 表示启动失败
 * @details 初始化流程：
 *          1. 配置日志系统（控制台输出 + 异步写入）
 *          2. 启动 HTTP 服务（端口 8080）
 *          3. 从数据库加载 SIP 配置
 *          4. 启动 PJSIP SIP 服务
 *          5. 启动心跳超时检查线程
 *          6. 等待终止信号（SIGINT）
 *          7. 优雅关闭各服务
 * @note 预览 INVITING 定时巡检（preview_invite_timeout_sec）已停用，避免误发 BYE；无 RTP 回收依赖 ZLM Hook 等其它路径。
 */
int main(int argc, char* argv[]) {
  Logger::Instance().add(std::make_shared<ConsoleChannel>());
  Logger::Instance().setWriter(std::make_shared<AsyncLogWriter>());

  gb::ensureUpstreamCatalogScopeTable();
  gb::ensureUpstreamCatalogCameraExcludeTable();

  gb::GetMediaService().reloadPreviewInviteTimeoutSec();
  // 与 HTTP 预览共用同一 MediaService：从 media_config 加载 ZLM media_api_url/secret，避免仅上级 INVITE、未点过预览时未 initialize
  if (!gb::GetMediaService().initialize()) {
    ErrorL << "MediaService initialize failed";
    return 1;
  }
  InfoL << "MediaService initialized (ZLM config 与预览同源 media_config)";

  TcpServer::Ptr server(new TcpServer());
  server->start<gb::HttpSession>(8080, "0.0.0.0");
  InfoL << "gb_service HTTP on 0.0.0.0:8080";

  // 从 gb_local_config 读取 SIP 服务配置
  gb::SipServerConfig sip_cfg = gb::loadSipServerConfig();
  InfoL << "SIP config loaded: port=" << sip_cfg.signal_port 
        << " UDP=" << (sip_cfg.transport_udp ? "yes" : "no")
        << " TCP=" << (sip_cfg.transport_tcp ? "yes" : "no");

  // 启动 PJSIP SIP 服务（根据配置自动选择 UDP/TCP）
  if (!gb::SipServerPjsipStart(sip_cfg)) {
    ErrorL << "SIP (PJSIP) start failed";
    return 1;
  }

  // 启动心跳超时检查（120秒超时，自动级联更新摄像头状态）
  gb::startHeartbeatTimeoutChecker(120);

  static semaphore sem;
#ifndef _WIN32
  signal(SIGINT, [](int) { g_running = false; sem.post(); });
#endif
  sem.wait();
  g_running = false;
  
  // 停止心跳超时检查
  gb::stopHeartbeatTimeoutChecker();

  gb::SipServerPjsipStop();

  return 0;
}
