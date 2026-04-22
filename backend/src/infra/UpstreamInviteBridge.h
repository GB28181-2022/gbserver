/**
 * @file UpstreamInviteBridge.h
 * @brief 上级平台入站 INVITE 点播：ID 映射、ZLM 收流、向下级 INVITE、startSendRtp 回上级
 */
#ifndef GB_SERVICE_UPSTREAM_INVITE_BRIDGE_H
#define GB_SERVICE_UPSTREAM_INVITE_BRIDGE_H

#include <pjsip.h>
#include <string>
#include <cstdint>

namespace gb {

struct UpstreamSignalMatch;

/** 下级 INVITE 发出后登记 y= SSRC，供上级侧 startSendRtp 使用 */
void upstreamBridgeRecordDeviceInviteSsrc(const std::string& deviceCallId, const std::string& ssrc);

/** processPendingInvites 中向下级发 INVITE 失败时调用 */
void upstreamBridgeOnDeviceInviteSendFailed(const std::string& deviceCallId);

/**
 * @brief 处理上级入站 INVITE（须在已匹配 enabled 上级时调用）
 * @return true 已处理（含已发错误响应）
 */
bool tryHandleUpstreamPlatformInvite(pjsip_rx_data* rdata, const UpstreamSignalMatch& um);

/** 收到下级 INVITE 的 200/非 200 响应时调用（先匹配 device Call-ID） */
void upstreamBridgeOnDeviceInviteRxResponse(pjsip_rx_data* rdata, int statusCode);

/** 上级 BYE / CANCEL（Call-ID 为上级会话） */
bool tryHandleUpstreamBye(pjsip_rx_data* rdata, const UpstreamSignalMatch& um);
bool tryHandleUpstreamCancel(pjsip_rx_data* rdata, const UpstreamSignalMatch& um);

/** PJSIP 工作线程周期调用：下级已 200 且等待 ZLM 源流时轮询 getMediaList，超时则 teardown */
void processUpstreamZlmSourceWaitPoll();

/**
 * @brief ZLM Hook on_rtp_server_timeout：openRtpServer 长期无 RTP 时由 ZLM 回调应用
 * @param streamId 与 openRtpServer 的 stream_id 一致
 * @return true 已匹配上级桥接并 teardown（答上级/下级 BYE/关会话）；false 无桥接（走预览等路径）
 */
bool upstreamBridgeTryTeardownForRtpServerTimeout(const std::string& streamId);

/**
 * @brief ZLM Hook on_send_rtp_stopped：startSendRtp 推流停止时回调
 * @param streamId 流标识
 * @param ssrc SSRC（可选）
 * @details 上级断流时触发：停止推流、向上级 BYE、向下级 BYE、清理桥接
 */
void upstreamBridgeOnZlmSendRtpStopped(const std::string& streamId, const std::string& ssrc);

/**
 * @brief 标记“上级刚发过 RecordInfo 检索”，供后续 INVITE 判定回放语义
 * @param ttlSec 关联窗口秒数（建议 30~60）
 */
void upstreamBridgeMarkReplayHint(int64_t upstreamDbId, const std::string& catalogGbId, int ttlSec);

}  // namespace gb

#endif
