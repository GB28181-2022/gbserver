/**
 * @file MediaService.h
 * @brief 媒体服务协调层 - ZLMediaKit HTTP API封装与流会话管理
 * @details 实现GB28181视频预览的媒体流协调功能：
 *          - ZLM HTTP API调用（openRtpServer/closeRtpServer）
 *          - 流会话生命周期管理（创建、维护、销毁）
 *          - 播放URL生成（Nginx代理模式，取系统配置流媒体IP）
 *          - Web Hook事件处理（on_stream_none_reader自动断流）
 *          
 *          协议关联：GB/T 28181-2022 第8章 媒体传输
 *          架构关联：架构设计.md 3.1 流媒体服务（对ZLM的协调层）
 * @date 2025
 * @note ZLM HTTP API端口：880（config.ini [http] port）
 *       播放URL格式：http://{media_ip}/zlm/rtp/{stream_id}.live.flv
 */

#ifndef GB_SERVICE_MEDIA_SERVICE_H
#define GB_SERVICE_MEDIA_SERVICE_H

#include <string>
#include <vector>
#include <memory>
#include <chrono>
#include <unordered_map>
#include <mutex>
#include <atomic>

namespace gb {

/**
 * @brief 流会话状态枚举
 * @details 流会话生命周期状态机：
 *          init -> inviting -> streaming -> closing -> closed
 */
enum class StreamSessionStatus {
    INIT = 0,       // 初始状态
    INVITING = 1, // SIP INVITE已发送，等待设备响应
    STREAMING = 2,// 流已建立，可播放
    CLOSING = 3,  // 正在关闭
    CLOSED = 4    // 已关闭
};

/**
 * @brief 获取状态名称字符串
 */
inline const char* getStreamStatusName(StreamSessionStatus status) {
    switch (status) {
        case StreamSessionStatus::INIT: return "init";
        case StreamSessionStatus::INVITING: return "inviting";
        case StreamSessionStatus::STREAMING: return "streaming";
        case StreamSessionStatus::CLOSING: return "closing";
        case StreamSessionStatus::CLOSED: return "closed";
        default: return "unknown";
    }
}

/**
 * @brief 流会话信息结构体
 * @details 存储单个视频流的完整会话信息
 */
struct StreamSession {
    std::string sessionId;              // 会话唯一标识（UUID）
    std::string streamId;               // ZLM流标识（平台国标ID_摄像头ID）
    std::string cameraId;               // 摄像头通道国标 ID（SIP 通道）
    std::string deviceGbId;             // 设备侧国标 ID（BYE/INFO 等，常与通道一致或平台域）
    std::string platformGbId;             // 所属平台国标ID
    std::string cameraDbId;             // cameras 表 BIGINT 主键（字符串），用于 stream_sessions 关联
    
    uint16_t zlmPort = 0;               // ZLM收流端口（RTP端口）
    std::string callId;                 // SIP通话标识（用于BYE）
    
    StreamSessionStatus status = StreamSessionStatus::INIT;
    std::string flvUrl;                 // HTTP-FLV播放地址（Nginx代理格式）
    std::string wsFlvUrl;               // WebSocket-FLV播放地址
    
    int viewerCount = 0;                // 观看者计数
    std::atomic<bool> isActive{false};  // 是否正在推流
    
    std::chrono::steady_clock::time_point createTime;
    std::chrono::steady_clock::time_point lastActivityTime;
    
    // 错误信息（如果失败）
    std::string errorMessage;
    int errorCode = 0;
};

/**
 * @brief ZLM HTTP API响应结构
 */
struct ZlmApiResponse {
    int code = -1;                      // ZLM返回码（0=成功）
    std::string message;                // 响应消息
    
    // openRtpServer成功时返回
    uint16_t port = 0;                  // ZLM分配的RTP收流端口
};

/**
 * @brief 媒体服务主类
 * @details 单例模式，提供ZLM协调和流会话管理功能
 */
class MediaService {
public:
    /**
     * @brief 获取单例实例
     */
    static MediaService& instance();

    /**
     * @brief 构建统一的ZLM流标识
     * @details 优先使用“平台国标ID_摄像头ID”，避免不同下级平台摄像头ID冲突
     * @param platformGbId 下级平台国标ID
     * @param cameraId 摄像头ID
     * @return ZLM流标识
     */
    static std::string buildStreamId(const std::string& platformGbId,
                                     const std::string& cameraId);
    
    /**
     * @brief 初始化服务
     * @param defaultApiBaseUrl ZLM HTTP API 根地址（默认 http://127.0.0.1:880）；若 media_config.media_api_url 非空则优先用库中值
     * @param zlmSecret ZLM API密钥；空则从 media_config 读取
     * @return 是否成功
     */
    bool initialize(const std::string& defaultApiBaseUrl = "http://127.0.0.1:880",
                    const std::string& zlmSecret = "");

    /**
     * @brief 保存流媒体配置后刷新 ZLM 连接参数（API 根地址、secret、对外媒体 IP）
     * @details 在 media_config 更新后调用，使已初始化的实例立即使用新地址
     */
    void refreshZlmRuntimeConfig();

    /**
     * @brief 从 media_config 重新加载「预览 INVITING 无流超时」秒数（并确保库表列存在）
     * @details 保存流媒体配置或进程启动后调用；合法范围 10～600，默认 45
     */
    void reloadPreviewInviteTimeoutSec();

    /**
     * @brief 当前配置的预览无 RTP 超时秒数（INVITING/INIT 未进 STREAMING 时生效）
     */
    int getPreviewInviteTimeoutSec() const;

    /**
     * @brief 下级 INVITE 200 后轮询 getMediaList 的本地最长等待（秒，后备上限）
     * @details openRtpServer 长期无 RTP 时，ZLM 通过 Hook on_rtp_server_timeout 通知应用（见 handleRtpServerTimeout），
     *          上级桥接在该回调中 teardown。本值仅作轮询后备，避免 Hook 未配置时无限等待；建议与 ZLM 收流超时可比量级。
     */
    int getZlmOpenRtpServerWaitSec() const;

    /** @brief 确保 media_config 含 zlm_open_rtp_server_wait_sec 并加载 */
    void reloadZlmOpenRtpServerWaitSec();

    /**
     * @brief 巡检 INVITING/INIT 超时：先发 SIP BYE（与 handlePreviewStop 一致）再 closeSession
     * @return 本次关闭的会话数
     * @details 针对信令 200/ACK 后长期无 RTP、ZLM 未 regist 的场景；errorMessage 置 preview_no_rtp_after_ack
     */
    size_t tickInvitingTimeouts();

    /**
     * @brief 调用 ZLM setServerConfig 写入 rtp_proxy.port_range（热更新，通常无需重启 ZLM）
     * @param zlmApiBaseUrl ZLM HTTP API 根地址（与 media_config.media_api_url 一致，已规范化）
     * @param secret ZLM API 密钥（须与 ZLM config.ini [api] secret 一致）
     * @param startPort RTP 端口区间起始
     * @param endPort RTP 端口区间结束
     * @param errMsg 失败时人类可读原因
     * @return 是否成功（HTTP 200 且 JSON code==0）
     * @note ZLM Wiki 配置键为 rtp_proxy.port_range，值为 start-end；若版本键名不同请以 getServerConfig 为准
     */
    static bool pushRtpProxyPortRangeToZlm(const std::string& zlmApiBaseUrl,
                                         const std::string& secret,
                                         int startPort,
                                         int endPort,
                                         std::string& errMsg);

    /**
     * @brief 规范化流媒体 API 根地址（补全 http(s)://、去掉尾部 /）
     */
    static std::string normalizeMediaApiUrl(const std::string& input);

    /**
     * @brief 确保 media_config 含 media_api_url，并将旧 media_http_port 迁入后删除该列
     */
    static bool ensureMediaApiUrlMigration();
    
    /**
     * @brief 关闭服务并清理所有会话
     */
    void shutdown();
    
    // ========== ZLM HTTP API调用 ==========
    
    /**
     * @brief 创建RTP收流服务器
     * @details 调用ZLM /index/api/openRtpServer 接口
     *          根据GB28181协议，为设备推流创建RTP接收端口
     * @param streamId 流标识（平台国标ID_摄像头ID）
     * @param outPort 输出：ZLM分配的收流端口
     * @param tcpMode ZLM openRtpServer：0=UDP，1=TCP 被动，2=TCP 主动（见项目架构说明）
     * @return 是否成功
     * @note ZLM API文档：https://github.com/zlmediakit/ZLMediaKit/wiki
     */
    bool openRtpServer(const std::string& streamId, uint16_t& outPort, int tcpMode = 0);
    
    /**
     * @brief 关闭RTP收流服务器
     * @details 调用ZLM /index/api/closeRtpServer 接口
     *          在无人观看或主动停止时释放端口资源
     * @param streamId 流标识
     * @return 是否成功
     */
    bool closeRtpServer(const std::string& streamId);

    /**
     * @brief 将已注册流以 PS-RTP 发往对端（ZLMediaKit startSendRtp）
     * @param outLocalPort ZLM 返回的本端 RTP 端口，用于上级 200 OK SDP
     */
    bool startSendRtpPs(const std::string& streamId,
                        const std::string& ssrc,
                        const std::string& dstIp,
                        uint16_t dstPort,
                        bool isUdp,
                        uint16_t& outLocalPort);

    /** 停止向对端推流（stopSendRtp；ssrc 空则停止该流全部 sender） */
    bool stopSendRtpForStream(const std::string& streamId, const std::string& ssrc = "");

    /**
     * @brief 检查ZLM中是否已存在该流（getMediaList）
     * @details 对 stream/vhost/app 做 form 编码；解析顶层 data 数组是否非空；失败时再以 vhost+app 全量列表在 JSON 中匹配
     *          \"stream\" 字段（日志见 【getMediaList】）。不按 schema 过滤，避免多路派生误判。
     * @param streamId 流标识（与 openRtpServer 的 stream_id 一致）
     */
    bool isStreamExistsInZlm(const std::string& streamId);

    /**
     * @brief ZLM MP4 录制（/index/api/startRecord）
     * @param streamId 流标识
     * @param customizedPath 自定义录制目录（空则使用 ZLM 默认路径）
     * @param maxSecond 单文件最大时长秒数（0 则使用 ZLM 默认值，推荐 36000 防分片）
     */
    bool startMp4Record(const std::string& streamId,
                        const std::string& customizedPath = "",
                        int maxSecond = 0);
    bool stopMp4Record(const std::string& streamId);

    /**
     * @brief 通过 ZLM API 获取录制文件列表（/index/api/getMp4RecordFile）
     * @param streamId 流标识
     * @param period 日期，完整格式如 "2026-03-30" 返回文件列表，前缀如 "2026-03" 返回日期文件夹列表
     * @param outRootPath [out] ZLM 返回的 rootPath（含磁盘绝对路径和 /www/ 部分）
     * @param outFiles [out] 文件名列表（仅文件名，不含路径）
     * @return API 调用成功且 code==0
     */
    bool getMp4RecordFile(const std::string& streamId,
                          const std::string& period,
                          std::string& outRootPath,
                          std::vector<std::string>& outFiles);

    /**
     * @brief 通过 ZLM API 删除录制目录（/index/api/deleteRecordDirectory）
     * @param streamId 流标识
     * @param period 日期如 "2026-03-30"（删除该日期下的录制文件）；空串删除整个流的录制目录
     */
    bool deleteRecordDirectory(const std::string& streamId,
                               const std::string& period = "");

    // ========== 播放URL生成 ==========
    
    /**
     * @brief 生成HTTP-FLV播放URL
     * @details URL格式：http://{media_ip}/zlm/rtp/{stream_id}.live.flv
     *          media_ip从系统配置media_config.media_http_host获取
     *          通过Nginx代理 /zlm/ 转发到ZLM 880端口
     * @param streamId ZLM流标识
     * @return FLV播放URL
     */
    std::string generateFlvUrl(const std::string& streamId);
    
    /**
     * @brief 生成WebSocket-FLV播放URL
     * @details URL格式：ws://{media_ip}/zlm/rtp/{stream_id}.live.flv
     * @param streamId ZLM流标识
     * @return WebSocket-FLV播放URL
     */
    std::string generateWsFlvUrl(const std::string& streamId);
    
    /**
     * @brief 获取流媒体服务器IP
     * @details 从media_config表读取media_http_host配置
     * @return 流媒体IP地址
     */
    std::string getMediaServerIp();

    /**
     * @brief 获取ZLM API密钥
     * @details 从media_config表读取zlm_secret配置
     * @return ZLM API密钥
     */
    std::string getZlmSecret();

    // ========== 流会话管理 ==========
    
    /**
     * @brief 创建新流会话
     * @param cameraId 摄像头ID
     * @param deviceGbId 设备国标ID
     * @param platformGbId 平台国标ID
     * @return 流会话指针（失败返回nullptr）
     */
    std::shared_ptr<StreamSession> createSession(const std::string& cameraId,
                                                  const std::string& deviceGbId,
                                                  const std::string& platformGbId,
                                                  const std::string& cameraDbId = "");

    /**
     * @brief 使用自定义 streamId 创建会话（回放/下载与实时预览流隔离）
     */
    std::shared_ptr<StreamSession> createSessionWithStreamId(const std::string& streamId,
                                                           const std::string& cameraId,
                                                           const std::string& deviceGbId,
                                                           const std::string& platformGbId,
                                                           const std::string& cameraDbId = "");

    /**
     * @brief 附着到ZLM中已经存在的流
     * @details 当后端重启或本地会话丢失，但ZLM中流仍存在时，
     *          直接补建本地会话并复用现有播放地址，不再重复发起INVITE。
     * @param cameraId 摄像头ID
     * @param deviceGbId 设备国标ID
     * @param platformGbId 平台国标ID
     * @param cameraDbId 库内 cameras.id（可选，用于 stream_sessions 写入）
     * @return 流会话指针（失败返回nullptr）
     */
    std::shared_ptr<StreamSession> attachToExistingStream(const std::string& cameraId,
                                                          const std::string& deviceGbId,
                                                          const std::string& platformGbId,
                                                          const std::string& cameraDbId = "");
    
    /**
     * @brief 获取流会话
     * @param sessionId 会话ID
     * @return 流会话指针（不存在返回nullptr）
     */
    std::shared_ptr<StreamSession> getSession(const std::string& sessionId);
    
    /**
     * @brief 通过streamId获取会话
     * @param streamId ZLM流标识
     * @return 流会话指针
     */
    std::shared_ptr<StreamSession> getSessionByStreamId(const std::string& streamId);

    /**
     * @brief 本级是否已有该 streamId 且会话为 STREAMING（HTTP 预览等已在本机拉流成功）
     * @details 与 handlePreviewStart 一致：不能仅依赖 ZLM getMediaList，否则漏检时会重复向下级发 INVITE。
     */
    bool isLocalStreamPlaying(const std::string& streamId) const;

    /**
     * @brief 关闭并移除流会话
     * @param sessionId 会话ID
     * @return 是否成功
     */
    bool closeSession(const std::string& sessionId);

    /**
     * @brief 关闭指定摄像头国标 ID 关联的所有本地流会话
     * @param cameraId 摄像头国标 ID（与 StreamSession.cameraId 一致）
     * @return 尝试关闭的会话数量（含已不存在的会话 ID）
     * @details 与 HttpSession::handlePreviewStop 一致：先发 SIP BYE，再 closeSession
     */
    size_t closeSessionsForCamera(const std::string& cameraId);
    
    /**
     * @brief 更新会话状态
     * @param sessionId 会话ID
     * @param status 新状态
     */
    void updateSessionStatus(const std::string& sessionId, StreamSessionStatus status);
    
    /**
     * @brief 设置会话SIP Call-ID
     * @param sessionId 会话ID
     * @param callId SIP通话标识
     */
    void setSessionCallId(const std::string& sessionId, const std::string& callId);
    
    /**
     * @brief 标记流为活跃状态（收到on_stream_changed hook）
     * @param streamId ZLM流标识
     */
    void markStreamActive(const std::string& streamId);
    
    /**
     * @brief 观看者连接（增加计数）
     * @param streamId ZLM流标识
     */
    void onViewerAttach(const std::string& streamId);
    
    /**
     * @brief 观看者断开（减少计数）
     * @param streamId ZLM流标识
     */
    void onViewerDetach(const std::string& streamId);
    
    /**
     * @brief 获取会话观看者数量
     * @param streamId ZLM流标识
     * @return 观看者计数
     */
    int getViewerCount(const std::string& streamId);
    
    // ========== Web Hook处理 ==========
    
    /**
     * @brief 处理ZLM Web Hook事件
     * @details 处理来自ZLM的回调事件：
     *          - on_stream_changed: 流注册/注销
     *          - on_stream_none_reader: 无人观看，触发自动断流
     * @param event 事件名称
     * @param streamId ZLM流标识
     * @param params 其他参数（JSON字符串）
     * @return 是否处理成功
     */
    bool handleZlmHook(const std::string& event, 
                       const std::string& streamId,
                       const std::string& params = "");
    
    /**
     * @brief 处理无人观看事件（on_stream_none_reader）
     * @details ZLM 已延时判定无人观看；发 SIP BYE（与 handleRtpServerTimeout 条件一致）并 closeSessionInternal。
     *          不以本地 viewerCount 拦截（前端未同步时会导致永不拆流）。
     * @param streamId ZLM 回调中的流标识（与 openRtpServer 的 stream_id 一致）
     * @return true=已处理并移除本地会话；false=无此 streamId 的会话（调用方可尝试其它候选）
     */
    bool handleStreamNoneReader(const std::string& streamId);
    
    /**
     * @brief 处理流状态变更事件
     * @param streamId ZLM流标识
     * @param isRegist true=流注册，false=流注销
     * @note regist=false：在 closeSessionInternal 之前排队 SIP BYE（与 ZLM 拆流顺序无关，避免
     *       仅 on_stream_none_reader 时会话已被删导致无法 BYE）。on_rtp_server_timeout 仍单独处理无 RTP。
     */
    /**
     * @return 是否在 streamIdIndex_ 中找到会话并处理（未找到则 Hook 与本地 stream_id 可能不一致）
     */
    bool handleStreamChanged(const std::string& streamId, bool isRegist);

    /**
     * @brief ZLM openRtpServer 长期无 RTP 时触发（Hook：on_rtp_server_timeout）
     * @details 若存在上级点播桥接（同 stream_id），先走 upstreamBridgeTryTeardownForRtpServerTimeout（答上级/下级 BYE）；
     *          否则与预览一致：必要时 sendPlayByeAsync，再 closeSessionInternal
     */
    void handleRtpServerTimeout(const std::string& streamId);
    
    // ========== 会话清理 ==========
    
    /**
     * @brief 清理过期会话
     * @details 定期清理超过最大生命周期的会话，防止内存泄漏
     * @param maxAgeSeconds 最大存活时间（秒）
     * @return 清理的会话数量
     */
    size_t cleanupExpiredSessions(int maxAgeSeconds = 300);
    
    /**
     * @brief 获取当前活动会话数量
     */
    size_t getActiveSessionCount() const;

private:
    MediaService() = default;
    ~MediaService() = default;
    MediaService(const MediaService&) = delete;
    MediaService& operator=(const MediaService&) = delete;
    
    /**
     * @brief 执行ZLM HTTP API请求
     * @param api API路径（如 /index/api/openRtpServer）
     * @param params 请求参数（JSON格式）
     * @param outResponse 输出响应
     * @return HTTP状态码（200成功）
     */
    int callZlmApi(const std::string& api, 
                   const std::string& params, 
                   std::string& outResponse);
    
    /**
     * @brief 生成唯一会话ID
     */
    std::string generateSessionId();
    
    /**
     * @brief 内部关闭会话（不带锁）
     */
    bool closeSessionInternal(const std::string& sessionId);
    
    // 配置
    std::string zlmApiBaseUrl_; // ZLM HTTP API 根地址（media_api_url）
    std::string zlmSecret_;     // ZLM密钥
    std::string mediaServerIp_; // 流媒体主机（media_http_host，播放/SDP）
    
    // 会话管理
    mutable std::mutex sessionsMutex_;
    std::unordered_map<std::string, std::shared_ptr<StreamSession>> sessions_;       // sessionId -> session
    std::unordered_map<std::string, std::shared_ptr<StreamSession>> streamIdIndex_;  // streamId -> session
    
    // 计数器
    std::atomic<uint64_t> sessionCounter_{0};
    
    bool initialized_ = false;

    /** @brief 预览会话在 INVITING/INIT 下等待 ZLM 收流的最长时间（秒） */
    std::atomic<int> previewInviteTimeoutSec_{45};

    /** @brief 上级桥接：下级 200 后轮询后备上限（秒）；无 RTP 的权威结束以 ZLM on_rtp_server_timeout 为准 */
    std::atomic<int> zlmOpenRtpServerWaitSec_{10};
};

/**
 * @brief 全局媒体服务实例访问函数
 */
MediaService& GetMediaService();

} // namespace gb

#endif // GB_SERVICE_MEDIA_SERVICE_H
