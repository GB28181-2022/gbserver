/**
 * @file SipCatalog.h
 * @brief GB28181 SIP 目录查询、点播异步 INVITE/BYE、云台 DeviceControl 入队等对外接口
 * @details 协议关联：MESSAGE 目录、INVITE 实时预览、MESSAGE DeviceControl（PTZCmd）。
 *          异步发送统一经 processPendingInvites() 在 PJSIP 工作线程执行。
 * @dependencies PJSIP（实现见 SipCatalog.cpp）、数据库 device_platforms / cameras
 * @date 2025
 * @note HTTP 线程禁止直接调用底层 PJSIP send；应使用 sendPlayInviteAsync、enqueuePtzDeviceControl 等
 */
#ifndef GB_SERVICE_SIP_CATALOG_H
#define GB_SERVICE_SIP_CATALOG_H

#include <cstdint>
#include <string>
#include <functional>
#include <vector>
#include <chrono>
#include <map>

namespace gb {

/**
 * @brief 目录节点类型枚举
 * @details 根据 GB28181-2016 设备编码规则，第11-13位表示类型编码：
 *          - 11x = 报警设备/目录组织
 *          - 12x = 视频设备（121=NVR, 131=摄像机, 132=IPC）
 *          - 13x = 视频设备（同12x，部分厂商使用）
 *          - 21x = 业务分组/虚拟组织/行政区划（215=业务分组, 216=虚拟组织, 其余 21x 多为行政区划）
 *          - 22x = 用户/角色相关
 */
enum class CatalogNodeType {
    DEVICE = 0,       // 设备（摄像头、NVR等视频设备）
    DIRECTORY = 1,    // 目录/组织节点（含 GB 业务分组 215、虚拟组织 216 等）
    REGION = 2        // 行政区域编码
};

/**
 * @brief 获取节点类型的字符串描述
 */
inline const char* getNodeTypeName(CatalogNodeType type) {
    switch (type) {
        case CatalogNodeType::DEVICE: return "设备";
        case CatalogNodeType::DIRECTORY: return "目录";
        case CatalogNodeType::REGION: return "行政区域";
        default: return "未知";
    }
}

/**
 * @brief 根据 DeviceID 第11-13位判断节点类型（支持3位编码）
 * @details GB28181-2016 设备编码规则（20位）：
 *          格式：3402000000 121 57000001
 *                 |前10位|类型|序号
 *          
 *          第11-13位类型编码（3位）：
 *          - 111 = 报警目录/组织节点 -> DIRECTORY
 *          - 112 = 报警设备 -> DEVICE
 *          - 121 = NVR网络硬盘录像机 -> DEVICE
 *          - 122 = DVR数字录像机 -> DEVICE
 *          - 131 = 摄像机 -> DEVICE
 *          - 132 = 网络摄像机(IPC) -> DEVICE
 *          - 215 = 业务分组（Business Group，ParentID 为系统） -> DIRECTORY
 *          - 216 = 虚拟组织（Virtual Organization，BusinessGroupID 归属业务分组） -> DIRECTORY
 *          - 其他 21x = 行政区划 -> REGION
 *          
 * @note 兼容处理：
 *          - 非20位编码：通过CivilCode字段补充判断
 *          - 部分厂商扩展编码：优先匹配3位类型码
 * @param deviceId 设备国标ID
 * @param civilCode 行政区划码（可选，用于辅助判断非标准编码）
 * @return 节点类型枚举
 */
inline CatalogNodeType getNodeTypeFromDeviceId(const std::string& deviceId, 
                                                const std::string& civilCode = "") {
    // 检查编码长度（标准GB28181编码为20位）
    if (deviceId.length() >= 13) {
        // 标准20位编码，解析第11-13位（3位类型编码）
        std::string typeCode3 = deviceId.substr(10, 3);
        
        // 报警设备/目录组织 (11x)
        if (typeCode3.substr(0, 2) == "11") {
            if (typeCode3 == "111") return CatalogNodeType::DIRECTORY;  // 报警目录
            return CatalogNodeType::DEVICE;  // 112+ 报警设备
        }
        
        // 视频设备 (12x, 13x)
        if (typeCode3.substr(0, 1) == "1" && 
            (typeCode3.substr(1, 1) == "2" || typeCode3.substr(1, 1) == "3")) {
            // 121=NVR, 122=DVR, 128=解码器, 131=摄像机, 132=IPC 等
            return CatalogNodeType::DEVICE;
        }
        
        // 业务分组 / 虚拟组织 / 行政区划 (21x)
        if (typeCode3.substr(0, 2) == "21") {
            if (typeCode3 == "215" || typeCode3 == "216") {
                return CatalogNodeType::DIRECTORY;  // 215=业务分组, 216=虚拟组织
            }
            return CatalogNodeType::REGION;  // 其他21x为行政区划
        }
        
        // 22x 用户角色相关 -> 归类为目录
        if (typeCode3.substr(0, 2) == "22") {
            return CatalogNodeType::DIRECTORY;
        }
    }
    
    // 非20位编码或无法识别的编码：尝试用前2位判断（向后兼容）
    if (deviceId.length() >= 12) {
        std::string typeCode2 = deviceId.substr(10, 2);
        // 注意：11x是报警设备，12x和13x是视频设备，14x是行政区划
        // 这里只处理明确的目录类型
        if (typeCode2 == "11") return CatalogNodeType::DIRECTORY;  // 11x报警目录
        if (typeCode2 == "14") return CatalogNodeType::REGION;     // 14x行政区划
        // 12、13 或其他默认为设备（包括121=NVR, 131=摄像机, 132=IPC）
    }
    
    // 极短编码（小于12位）：如果有civilCode，可能为行政区划节点
    if (deviceId.length() < 12 && !civilCode.empty()) {
        // 通过civilCode辅助判断，通常表示行政区域
        return CatalogNodeType::REGION;
    }
    
    // 默认作为设备处理
    return CatalogNodeType::DEVICE;
}

/**
 * @brief 获取设备类型编码的详细描述（用于日志和调试）
 * @param deviceId 设备国标ID
 * @return 类型描述字符串
 */
inline std::string getDeviceTypeDescription(const std::string& deviceId) {
    if (deviceId.length() < 13) return "未知/短编码";
    
    std::string typeCode3 = deviceId.substr(10, 3);
    
    // 报警设备类
    if (typeCode3 == "111") return "报警目录/组织";
    if (typeCode3 == "112") return "报警设备";
    
    // 视频设备类
    if (typeCode3 == "121") return "NVR网络硬盘录像机";
    if (typeCode3 == "122") return "DVR数字录像机";
    if (typeCode3 == "128") return "视频解码器";
    if (typeCode3 == "131") return "摄像机";
    if (typeCode3 == "132") return "网络摄像机(IPC)";
    if (typeCode3 == "133") return "球机";
    if (typeCode3 == "134") return "云台摄像机";
    
    // GB28181 目录项：业务分组 / 虚拟组织
    if (typeCode3 == "215") return "业务分组(215)";
    if (typeCode3 == "216") return "虚拟组织(216)";
    
    // 行政区划类
    if (typeCode3.substr(0, 2) == "21") return "行政区划/区域(21x)";
    
    // 用户角色类
    if (typeCode3.substr(0, 2) == "22") return "用户/角色";
    
    return "其他设备(" + typeCode3 + ")";
}

/**
 * @brief 判定是否“可预览摄像头”类型
 * @details 当前业务规则：仅 131/132 归入摄像头；如 181/200 等设备类节点不作为摄像头处理。
 */
inline bool isPreviewCameraTypeFromDeviceId(const std::string& deviceId) {
    if (deviceId.length() < 13) return false;
    std::string typeCode3 = deviceId.substr(10, 3);
    return typeCode3 == "131" || typeCode3 == "132";
}

/**
 * @brief 目录节点信息结构体
 * @details 支持设备、目录、行政区域三种类型，统一存储在 catalog_nodes 表
 */
struct CatalogNodeInfo {
    // 基本字段（所有类型都有）
    std::string nodeId;           // 节点国标ID（DeviceID）
    std::string name;             // 节点名称
    std::string parentId;         // 父节点ID（ParentID）
    CatalogNodeType nodeType;       // 节点类型
    std::string civilCode;        // 行政区划码
    int parental = 0;             // 是否有子设备（0=无, 1=有）
    
    // 设备特有字段（nodeType = DEVICE 时有效）
    std::string manufacturer;     // 设备厂商
    std::string model;            // 设备型号
    std::string owner;            // 设备归属
    std::string address;          // 安装地址
    int safetyWay = 0;            // 信令安全模式
    int registerWay = 1;          // 注册方式
    std::string secrecy = "0";    // 保密属性
    std::string status;           // 设备状态
    double longitude = 0.0;       // 经度
    double latitude = 0.0;        // 纬度
    
    // GB28181-2016 扩展字段（设备安全相关）
    std::string block;            // 封锁状态（ON/OFF）
    std::string certNum;          // 证书编号
    int certifiable = 0;          // 证书有效性（0=无效, 1=有效）
    std::string errCode;          // 错误码
    std::string errTime;          // 错误时间
    std::string ipAddress;        // 设备IP地址
    int port = 0;                 // 设备端口
    
    // 目录/行政区域特有字段
    int itemNum = 0;              // 子节点数量（DeviceList Num）
    int itemIndex = -1;           // 当前节点在设备列表中的索引（Item Index）
    std::string businessGroupId;  // 业务分组ID（BusinessGroupID）
    
    // 辅助方法：是否为设备类节点（目录树层面的 DEVICE）
    bool isDevice() const { return nodeType == CatalogNodeType::DEVICE; }
    // 辅助方法：是否为可预览摄像头（仅 131/132）
    bool isCamera() const { return isPreviewCameraTypeFromDeviceId(nodeId); }
    bool isDirectory() const { return nodeType == CatalogNodeType::DIRECTORY; }
    bool isRegion() const { return nodeType == CatalogNodeType::REGION; }
};

/**
 * @brief Catalog 会话类，用于处理分页上报
 * @details 下级平台可能分多次 Notify 上报所有设备，此类用于缓存和聚合数据
 */
class CatalogSession {
public:
    std::string platformGbId;       // 平台GBID
    int sn;                         // 命令序列号
    int expectedTotal;              // SumNum 期望总数
    int receivedCount;              // 已接收数量
    std::vector<CatalogNodeInfo> allNodes;  // 所有节点
    std::chrono::steady_clock::time_point lastUpdate;  // 最后更新时间
    bool isSaved = false;             // 是否已保存到数据库
    
    CatalogSession(const std::string& gbId, int sn, int total)
        : platformGbId(gbId), sn(sn), expectedTotal(total), 
          receivedCount(0), lastUpdate(std::chrono::steady_clock::now()) {}
    
    // 添加节点并更新计数
    void addNodes(const std::vector<CatalogNodeInfo>& nodes);
    
    // 检查是否接收完成
    bool isComplete() const { return receivedCount >= expectedTotal; }
    
    // 检查是否超时（默认30秒）
    bool isTimeout(int seconds = 30) const;
    
    // 保存到数据库
    void saveToDatabase(int platformId);
};

// 设备信息结构体 - 符合 GB28181-2016 附录C 设备目录查询要求（保持兼容性）
struct CameraInfo {
  // 必选字段
  std::string deviceId;       // 设备ID (DeviceID)
  std::string name;           // 设备名称 (Name)
  std::string manufacturer;   // 设备厂商 (Manufacturer)
  std::string model;          // 设备型号 (Model)
  std::string owner;          // 设备归属 (Owner)，默认"0"
  std::string civilCode;      // 行政区划码 (CivilCode)，6位数字
  std::string address;        // 安装地址 (Address)
  int parental = 0;           // 是否有子设备 (Parental)，0-无，1-有
  std::string parentId;       // 父设备ID (ParentID)
  int safetyWay = 0;          // 信令安全模式 (SafetyWay)，0-不采用，1-采用
  int registerWay = 1;        // 注册方式 (RegisterWay)，1-RFC3261，2-GB28181
  std::string secrecy = "0";  // 保密属性 (Secrecy)，0-不涉密，1-涉密
  std::string status;         // 设备状态 (Status)，OK/ERROR/OFFLINE/ON/OFF

  // 可选字段
  double longitude = 0.0;     // 经度 (Longitude)，-180.0 ~ 180.0
  double latitude = 0.0;      // 纬度 (Latitude)，-90.0 ~ 90.0

  // 转换后的布尔在线状态
  bool online = false;        // 在线状态，由 status 字段映射：OK/ON -> true，其他 -> false
};

// Catalog 查询回调函数类型
using CatalogResponseCallback = std::function<void(const std::string& platformGbId, 
                                                    const std::vector<CameraInfo>& cameras,
                                                    int totalSum)>;

/**
 * 发送 Catalog 查询请求（Query）
 * @param platformGbId 目标平台GBID
 * @param sn 命令序列号（建议用时间戳或递增序列）
 * @return 是否发送成功
 * 注意：此函数必须在 PJSIP 工作线程中调用
 */
bool sendCatalogQuery(const std::string& platformGbId, int sn);

/**
 * 异步发送 Catalog 查询请求（Query）
 * 适用于从非 PJSIP 线程调用（如 HTTP 线程）
 * @param platformGbId 目标平台GBID
 * @param sn 命令序列号（建议用时间戳或递增序列）
 * @param forceEnqueue true 时忽略等待中/进行中的去重，适用于页面手动触发
 * @return 是否成功加入发送队列
 */
bool sendCatalogQueryAsync(const std::string& platformGbId, int sn, bool forceEnqueue = false);

/**
 * 处理待发送的 Catalog 查询请求
 * 应该在 PJSIP 工作线程中定期调用
 */
void processPendingCatalogQueries();

/**
 * @brief 消费待发送的国标录像目录查询（RecordInfo）队列
 * @note 须在 PJSIP worker 中与 processPendingCatalogQueries 一样周期调用
 */
void processPendingRecordInfoQueries();
/**
 * @brief 入队一条录像查询（供上级级联转发复用）
 * @note 仅入队，不阻塞等待结果；需在 PJSIP worker 中调用 processPendingRecordInfoQueries 发送
 */
bool enqueueRecordInfoQuery(const std::string& platformGbId,
                            const std::string& channelId,
                            const std::string& startTimeGb,
                            const std::string& endTimeGb,
                            int sn);

/**
 * @brief 向上级信令地址发送 MANSCDP+xml
 * @param sipMethod SIP 方法名："MESSAGE"（默认，保活/PTZ 等）或 "NOTIFY"（目录推送）
 * @note 须在 PJSIP worker 线程调用；UDP 目的使用 destSignalIp:destSignalPort
 */
bool sendUpstreamManscdpMessage(const std::string& destSignalIp,
                                int destSignalPort,
                                const std::string& toUser,
                                const std::string& toHost,
                                int toPort,
                                const std::string& xmlBody,
                                const std::string& sipMethod = "MESSAGE");

/**
 * @brief 收到下级 RecordInfo 响应（MESSAGE + Response）时解析并写入 replay_segments
 */
void handleRecordInfoMessage(const std::string& fromPlatformGbId, const std::string& body);

/**
 * @brief 阻塞触发一次录像检索（HTTP 线程调用）：入队 SIP、等待响应或超时
 * @param platformGbIdOpt 为空则从 cameras.platform_gb_id 解析
 * @param outError 失败原因
 * @return 成功写入数据库并返回 true
 */
bool runRecordInfoSearchBlocking(const std::string& cameraId,
                                 const std::string& platformGbIdOpt,
                                 const std::string& startTime,
                                 const std::string& endTime,
                                 std::string& outError);

/**
 * @brief 解析 HTTP 路径中的摄像头键：≤12 位纯数字视为 cameras.id，否则按 device_gb_id 查（可多平台时用 platformGbIdOpt 消歧）
 * @param outDbId cameras.id 字符串
 * @param outDeviceGbId 下级国标 DeviceID（SIP 通道）
 * @param outPlatformGbId 平台国标域
 */
bool resolveCameraRowByPathSegment(const std::string& segment,
                                   const std::string& platformGbIdOpt,
                                   std::string& outDbId,
                                   std::string& outDeviceGbId,
                                   std::string& outPlatformGbId);

/**
 * 解析 Catalog 查询响应（Notify）
 * @param body SIP MESSAGE body (XML)
 * @param cameras 输出解析后的设备列表
 * @param totalSum 输出设备总数
 * @return 是否解析成功
 */
bool parseCatalogResponse(const std::string& body, 
                          std::vector<CameraInfo>& cameras,
                          int& totalSum);

/**
 * 保存设备信息到数据库
 * @param platformId 平台数据库ID
 * @param platformGbId 平台GBID
 * @param cameras 设备列表
 * @return 保存的设备数量
 */
int saveCamerasToDb(int platformId, const std::string& platformGbId, 
                    const std::vector<CameraInfo>& cameras);

/**
 * 设置 Catalog 响应回调
 * 当收到 Catalog Notify 时调用
 */
void setCatalogResponseCallback(CatalogResponseCallback callback);

/**
 * 内部函数：当收到 Catalog Notify 时调用
 * 注意：此函数被 SipServerPjsip 调用
 */
void handleCatalogNotify(const std::string& platformGbId, const std::string& body);

/**
 * 获取 Catalog 查询状态
 */
bool isCatalogQueryInProgress(const std::string& platformGbId);

/**
 * @brief 解析 Catalog 响应为目录节点列表（支持设备、目录、行政区域）
 * @param body SIP MESSAGE body (XML)
 * @param nodes 输出解析后的节点列表
 * @param totalSum 输出设备总数
 * @return 是否解析成功
 */
bool parseCatalogNodes(const std::string& body, 
                       std::vector<CatalogNodeInfo>& nodes,
                       int& totalSum);

/**
 * @brief 保存目录节点到数据库
 * @param platformId 平台数据库ID
 * @param platformGbId 平台GBID
 * @param nodes 节点列表
 * @return 保存的节点数量
 */
int saveCatalogNodesToDb(int platformId, const std::string& platformGbId, 
                          const std::vector<CatalogNodeInfo>& nodes);

/**
 * @brief 处理 Catalog Notify（新接口，支持目录树）
 * @param platformGbId 平台GBID（From头）
 * @param body SIP MESSAGE body
 * @param sn 命令序列号（用于匹配分页）
 */
void handleCatalogTreeNotify(const std::string& platformGbId, 
                              const std::string& body, 
                              int sn);

/**
 * @brief 清理过期的 Catalog Session
 * @param maxAgeSeconds 最大存活时间（秒）
 */
void cleanupCatalogSessions(int maxAgeSeconds = 300);

// ========== 视频预览（点播）功能 ==========

/**
 * @brief 发送点播INVITE请求
 * @details 根据GB28181-2022 第7章，发起实时视频点播请求：
 *          1. 构造SDP携带ZLM收流地址和端口
 *          2. 发送INVITE到设备
 *          3. 等待200 OK响应
 *          
 * SDP格式示例：
 * v=0
 * o=- 0 0 IN IP4 {local_ip}
 * s=Play
 * c=IN IP4 {local_ip}
 * t=0 0
 * m=video {zlm_port} RTP/AVP 96
 * a=rtpmap:96 PS/90000
 * a=recvonly
 *
 * @param deviceGbId 设备国标ID
 * @param channelId 通道ID（摄像头ID）
 * @param zlmPort ZLM收流端口
 * @param outCallId SIP Call-ID：若已由 sendPlayInviteAsync 预填则沿用；若为空则本函数内生成
 * @param outSdp 输出设备的SDP响应
 * @param sdpConnectionIp SDP 中 o=/c= 的 IN IP4 地址（空则回退为信令 signal_ip）
 * @param rtpOverTcp true 时 m= 行使用 TCP/RTP/AVP（与 ZLM tcp_mode=1 一致）
 * @return 是否成功
 */
bool sendPlayInvite(const std::string& deviceGbId,
                    const std::string& channelId,
                    uint16_t zlmPort,
                    std::string& outCallId,
                    std::string& outSdp,
                    const std::string& sdpConnectionIp,
                    bool rtpOverTcp,
                    uint64_t playbackStartUnix = 0,
                    uint64_t playbackEndUnix = 0,
                    bool isDownload = false);

/**
 * @brief 异步发送点播INVITE请求
 * @details 将INVITE请求加入队列，由PJSIP工作线程实际发送
 *          适用于从非PJSIP线程调用（如HTTP线程）
 * @param deviceGbId 设备国标ID
 * @param channelId 通道ID（摄像头ID）
 * @param zlmPort ZLM收流端口
 * @param outCallId 输出SIP Call-ID（立即返回，供后续使用）
 * @param sdpConnectionIp SDP c=/o= 连接地址（空则回退为信令 signal_ip）
 * @param rtpOverTcp true 时 SDP 使用 TCP/RTP/AVP（与 ZLM tcp_mode=1 一致）
 * @return 是否成功加入发送队列
 */
bool sendPlayInviteAsync(const std::string& deviceGbId,
                         const std::string& channelId,
                         uint16_t zlmPort,
                         std::string& outCallId,
                         const std::string& sdpConnectionIp,
                         bool rtpOverTcp,
                         uint64_t playbackStartUnix = 0,
                         uint64_t playbackEndUnix = 0,
                         bool isDownload = false);

/** 与 sendPlayInviteAsync 相同，但 Call-ID 由调用方指定（上级点播桥接） */
bool enqueuePlayInviteWithCallId(const std::string& platformGbId,
                                 const std::string& channelId,
                                 uint16_t zlmPort,
                                 const std::string& callId,
                                 const std::string& sdpConnectionIp,
                                 bool rtpOverTcp,
                                 uint64_t playbackStartUnix = 0,
                                 uint64_t playbackEndUnix = 0,
                                 bool isDownload = false);

/**
 * @brief 消费待发送队列：云台 MESSAGE、点播 INVITE、BYE
 * @details 须在 PJSIP 工作线程中调用（与 SipServerPjsip worker 绑定）。
 *          处理顺序保证 PTZ 与媒体会话信令在单线程内串行，降低竞态。
 */
void processPendingInvites();

/**
 * @brief 云台控制入队（HTTP 等线程调用，由 processPendingInvites 在 PJSIP 线程发 MESSAGE）
 * @param platformGbId 下级平台国标 ID（与点播 INVITE 一致）
 * @param channelId 通道/摄像机国标 ID
 * @param command up|down|left|right|zoomIn|zoomOut|irisOpen|irisClose|stop
 * @param action start|stop
 * @param speed 1–3
 * @return 入队成功 true；参数非法、编码失败或 SIP 未就绪为 false
 * @note 与 POST /api/ptz 的 JSON 字段一一对应；实际 PTZCmd 编码在 SipCatalog.cpp
 */
bool enqueuePtzDeviceControl(const std::string& platformGbId,
                             const std::string& channelId,
                             const std::string& command,
                             const std::string& action,
                             int speed);

/**
 * @brief 云台控制入队（PTZCmd HEX 透传）
 * @param platformGbId 下级平台国标 ID
 * @param channelId 通道/摄像机国标 ID
 * @param ptzCmdHex 16 位十六进制 PTZCmd（大小写均可）
 * @return 入队成功 true；参数非法或 SIP 未就绪为 false
 */
bool enqueuePtzDeviceControlHex(const std::string& platformGbId,
                                const std::string& channelId,
                                const std::string& ptzCmdHex);

/**
 * @brief 发送BYE请求停止点播（内部函数，仅在PJSIP线程调用）
 * @details 结束视频会话，停止设备推流
 * @param callId SIP通话标识
 * @param deviceGbId 设备国标ID（用于获取设备IP）
 * @param channelId 通道ID（摄像头ID，用于构建To头）
 * @return 是否成功
 */
bool sendPlayBye(const std::string& callId,
                 const std::string& deviceGbId,
                 const std::string& channelId);

/**
 * @brief 异步发送BYE请求停止点播
 * @details 将BYE请求加入队列，由PJSIP工作线程实际发送
 *          适用于从非PJSIP线程调用（如HTTP线程）
 * @param callId SIP通话标识
 * @param deviceGbId 设备国标ID
 * @param channelId 通道ID（摄像头ID），用于构建To头
 * @return 是否成功加入发送队列
 */
bool sendPlayByeAsync(const std::string& callId,
                      const std::string& deviceGbId,
                      const std::string& channelId);

/**
 * @brief 发送回放倍速控制 SIP INFO（MANSRTSP PLAY + Scale）
 * @details GB28181-2016 Annex A.2.5：通过 SIP INFO 在已有回放会话中切换倍速。
 *          仅在 PJSIP 线程调用（由 processPendingInvites 消费）。
 * @param callId 回放 INVITE 会话 Call-ID
 * @param deviceGbId 下级平台/设备国标 ID
 * @param channelId 通道 ID
 * @param scale 倍速值（0.25/0.5/1/2/4/8/16/32）
 * @return 是否发送成功
 */
bool sendPlaybackSpeedInfo(const std::string& callId,
                           const std::string& deviceGbId,
                           const std::string& channelId,
                           double scale);

/**
 * @brief 异步入队回放倍速 INFO（供 HTTP 等线程调用）
 * @details 同一 callId 在队列中去重，仅保留最新 scale。
 * @param callId 回放会话 Call-ID
 * @param deviceGbId 下级平台/设备国标 ID
 * @param channelId 通道 ID
 * @param scale 目标倍速
 * @return 入队成功 true
 */
bool sendPlaybackSpeedInfoAsync(const std::string& callId,
                                const std::string& deviceGbId,
                                const std::string& channelId,
                                double scale);

/**
 * @brief 获取平台侧优先用于寻址的 IP（signal_src 优先，否则 contact）
 * @details 供外部或调试使用；与点播/目录模块读库策略一致
 */
std::string getDeviceIp(const std::string& deviceGbId);

/**
 * @brief 处理INVITE响应
 * @details 当收到设备200 OK响应时调用，提取SDP信息
 * @param callId SIP通话标识
 * @param sdpBody SDP内容
 * @param isSuccess 是否成功（200 OK）
 */
void handleInviteResponse(const std::string& callId,
                          const std::string& sdpBody,
                          bool isSuccess);

}  // namespace gb

#endif  // GB_SERVICE_SIP_CATALOG_H
