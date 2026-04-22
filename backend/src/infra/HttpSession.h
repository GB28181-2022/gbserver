/**
 * @file HttpSession.h
 * @brief HTTP 会话处理模块
 * @details 基于 ZLToolKit 的 HTTP 会话管理：
 *          - RESTful API 接口实现
 *          - 设备/平台/摄像头/告警/目录管理、云台 /api/ptz
 *          - 认证与授权（JWT Token）
 *          - 配置管理
 *          遵循 Reactor 网络模型设计
 * @date 2025
 * @note 依赖 ZLToolKit 网络库
 */
#ifndef GB_SERVICE_HTTP_SESSION_H
#define GB_SERVICE_HTTP_SESSION_H

#include "Network/TcpServer.h"
#include "Network/Session.h"
#include "Network/Buffer.h"
#include "Util/logger.h"
#include <string>

namespace gb {

/**
 * @class HttpSession
 * @brief HTTP 会话类
 * @details 继承 ZLToolKit Session，处理 HTTP 请求：
 *          - 接收 HTTP 请求数据
 *          - 解析请求行、头部、Body
 *          - 路由到对应处理函数
 *          - 发送 JSON/XML 响应
 */
class HttpSession : public toolkit::Session {
 public:
  /**
   * @brief 构造函数
   * @param sock Socket 连接
   */
  explicit HttpSession(const toolkit::Socket::Ptr& sock);
  
  /**
   * @brief 析构函数
   */
  ~HttpSession() override = default;

  /**
   * @brief 接收数据回调
   * @param buf 接收到的数据缓冲区
   * @details ZLToolKit 框架回调，收到数据后解析处理
   */
  void onRecv(const toolkit::Buffer::Ptr& buf) override;
  
  /**
   * @brief 错误回调
   * @param err Socket 异常
   * @details 连接断开或出错时调用
   */
  void onError(const toolkit::SockException& err) override;
  
  /**
   * @brief 定时管理回调
   * @details 可用于超时检查，当前为空实现
   */
  void onManager() override;

 private:
  std::string recvBuffer_;

  // 请求处理函数
  static bool isGetApiHealth(const char* data, size_t len);
  void handleRequest(const char* data, size_t len);
  
  // 响应发送函数
  void sendJson(const std::string& body);
  /** @brief 按 HTTP 状态返回 JSON body（用于 catalog-group 等需 409/404 语义的路径） */
  void sendHttpJson(int httpStatus, const std::string& body);
  void sendJsonError(int code, const std::string& message);
  void sendNotFound();
  void sendFileDownload(const std::string& absolutePath, const std::string& downloadFileName);
  
  // 健康与配置 API
  void sendHealthResponse();
  void sendConfigLocalGb();
  void sendConfigMedia();
  void handlePutConfigLocalGb(const std::string& body);
  void handlePutConfigMedia(const std::string& body);
  
  // 认证 API
  void handleAuthLogin(const std::string& body);
  void handleAuthMe(const std::string& authHeader);
  void handleAuthLogout(const std::string& authHeader);
  void handleAuthChangePassword(const std::string& body, const std::string& authHeader);
  
  // 上级平台 API
  void sendPlatforms(const std::string& query);
  void sendPlatformById(const std::string& idStr);
  void sendPlatformCatalogScope(const std::string& idStr);
  void handlePostPlatform(const std::string& body);
  void handlePutPlatform(const std::string& idStr, const std::string& body);
  void handleDeletePlatform(const std::string& idStr);
  void handlePutPlatformCatalogScope(const std::string& idStr, const std::string& body);
  void handlePostPlatformCatalogNotify(const std::string& idStr);
  
  // 下级设备平台 API
  void sendDevicePlatforms(const std::string& query);
  void handlePostDevicePlatform(const std::string& body);
  void handlePutDevicePlatform(const std::string& idStr, const std::string& body);
  void handleDeleteDevicePlatform(const std::string& idStr);
  void handleCatalogQuery(const std::string& platformId);
  
  // 摄像头 API
  void sendCameras(const std::string& query);
  void handleDeleteCamera(const std::string& cameraId);
  void handleBatchDeleteCameras(const std::string& body);
  
  // 目录 API
  void sendCatalogNodes(const std::string& query);
  void sendCatalogNodeCameras(const std::string& nodeId);
  void sendPlatformCatalogTree(const std::string& query);  // 获取平台目录树
  void handlePostCatalogNode(const std::string& body);
  void handlePutCatalogNode(const std::string& idStr, const std::string& body);
  void handleDeleteCatalogNode(const std::string& idStr);
  void handlePutCatalogNodeCameras(const std::string& idStr, const std::string& body);
  
  // 告警 API
  void sendAlarms(const std::string& query);
  void handlePostAlarm(const std::string& body);
  void handlePutAlarm(const std::string& idStr, const std::string& body);
  
  // 统计 API
  void sendOverview();
  
  // 录像回放 API
  void sendReplaySegments(const std::string& query);
  void handleReplayStart(const std::string& cameraId, const std::string& body);
  void handleReplayStop(const std::string& cameraId, const std::string& body);
  /** POST /api/cameras/{cameraId}/replay/speed  回放倍速控制（SIP INFO + MANSRTSP） */
  void handleReplaySpeed(const std::string& cameraId, const std::string& body);
  void sendReplaySessionStatus(const std::string& cameraId, const std::string& query);
  void handleReplayDownloadPost(const std::string& body);
  void sendReplayDownloadStatus(const std::string& idStr);
  void sendReplayDownloadFile(const std::string& idStr);
  /** POST /api/replay/download/{id}/cancel 取消进行中的下载任务 */
  void handleReplayDownloadCancel(const std::string& idStr);
  /** POST /api/replay/download/{id}/cleanup 浏览器下载完成后删除 ZLM 录制文件 */
  void handleReplayDownloadCleanup(const std::string& idStr);
  /** POST /api/cameras/{cameraId}/data/clear 清空该摄像头关联的录像缓存、媒体会话行、告警等（不删 cameras 行） */
  void handleClearCameraRelatedData(const std::string& cameraId);

  // 视频预览（点播）API
  void handlePreviewStart(const std::string& cameraId, const std::string& body);
  void handlePreviewStop(const std::string& cameraId, const std::string& body);
  void sendPreviewSessionStatus(const std::string& cameraId, const std::string& query);
  /** @brief POST /api/ptz 请求体解析与入队，见 HttpSession.cpp */
  void handlePtzControl(const std::string& body);
  // ZLM Web Hook - 通过URL路径区分事件类型
  void handleZlmHookNoneReader(const std::string& body);      // on_stream_none_reader
  void handleZlmHookStreamChanged(const std::string& body);     // on_stream_changed
  void handleZlmHookRtpServerTimeout(const std::string& body);  // on_rtp_server_timeout
  void handleZlmHookSendRtpStopped(const std::string& body);    // on_send_rtp_stopped
};

}

#endif
