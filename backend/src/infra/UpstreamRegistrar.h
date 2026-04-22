/**
 * @file UpstreamRegistrar.h
 * @brief 向上级 SIP REGISTER（PJSIP regc）、保活、源地址与 upstream 匹配缓存
 */
#ifndef GB_SERVICE_UPSTREAM_REGISTRAR_H
#define GB_SERVICE_UPSTREAM_REGISTRAR_H

#include <cstdint>

namespace gb {

/** @brief 全量从库重建路由与 REGISTER（SIP 栈启动时调用） */
void requestUpstreamRegistrarReload();

/**
 * @brief 仅同步单条上级平台配置（新增/编辑保存后调用，避免全表 destroy+重注册）
 * @param id upstream_platforms.id
 */
void requestUpstreamRegistrarSyncRow(int64_t id);

/** @brief 移除单条上级平台的路由与 regc（删除配置后调用） */
void requestUpstreamRegistrarRemoveRow(int64_t id);

/** @brief 在 PJSIP worker 中周期调用：重载注册器、保活 MESSAGE */
void upstreamRegistrarProcessMaintenance();

struct UpstreamSignalMatch {
  int64_t platformDbId{0};
  bool matched{false};
  bool enabled{true};
};

/** @brief 按信令源 IP/端口匹配 upstream_platforms（与配置 sip_ip、sip_port 一致） */
UpstreamSignalMatch matchUpstreamBySignalSource(const char* srcIp, int srcPort);
/** @brief 按上级国标 ID（upstream_platforms.gb_id）匹配平台 */
UpstreamSignalMatch matchUpstreamByGbId(const char* gbId);

/**
 * @brief 主动向上级发送编组目录 NOTIFY 体
 * @note 内部入队，在 worker 发送；可从 HTTP 线程调用
 */
bool enqueueUpstreamCatalogNotify(int64_t upstreamDbId);

/**
 * @brief 上级目录检索应答：先回 200 OK 后，再用 MESSAGE 分包发送 Response XML
 * @param sn 上级查询的 SN，应答中需原样回传
 */
void enqueueUpstreamCatalogQueryResponse(int64_t upstreamDbId, int sn);

}  // namespace gb

#endif
