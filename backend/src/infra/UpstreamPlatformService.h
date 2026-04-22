/**
 * @file UpstreamPlatformService.h
 * @brief 上级平台：编组目录 XML、对外 ID 映射、scope 辅助
 * @details GB28181 平台级联；与 upstream_catalog_scope、catalog_group_* 对齐
 */
#ifndef GB_SERVICE_UPSTREAM_PLATFORM_SERVICE_H
#define GB_SERVICE_UPSTREAM_PLATFORM_SERVICE_H

#include <string>
#include <vector>

namespace gb {

/**
 * @brief 构建上级目录查询应答 XML（MANSCDP+xml）
 */
bool buildUpstreamCatalogResponseXml(long long upstreamPlatformId,
                                     const std::string& queryDeviceId,
                                     int sn,
                                     std::string& outXml);

/**
 * @brief 主动目录上报：生成多段 MANSCDP+xml（含 SumNum；目录/通道过多时分包）
 * @param ioSn 首个报文 SN，返回时已递增为「下一段可用 SN」
 */
bool buildUpstreamCatalogNotifyXmlParts(long long upstreamPlatformId, int& ioSn, std::vector<std::string>& outParts);

/**
 * @brief (upstream_db_id, 编组对外 DeviceID) → 内部 cameras.device_gb_id 与所属下级平台国标 ID
 * @return false 无映射
 */
bool resolveUpstreamCatalogDeviceId(long long upstreamPlatformId,
                                    const std::string& catalogGbDeviceId,
                                    std::string& outDeviceGbId,
                                    std::string& outPlatformGbId,
                                    long long& outCameraDbId);

std::string xmlEscapeText(const std::string& s);

}  // namespace gb

#endif
