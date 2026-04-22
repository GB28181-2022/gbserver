/**
 * @file CatalogGroupService.h
 * @brief 本机目录编组 HTTP 业务：catalog_group_* 表、20 位编码、导入去重
 * @details 与 GB28181 目录语义对齐；不修改 cameras.device_gb_id
 * @date 2026
 */
#ifndef GB_SERVICE_CATALOG_GROUP_SERVICE_H
#define GB_SERVICE_CATALOG_GROUP_SERVICE_H

#include <string>

namespace gb {

struct CatalogGroupHttpResult {
  int httpStatus{500};
  std::string jsonBody;
};

/**
 * @brief 分发 /api/catalog-group/*（不含路由匹配，由调用方传入 method+path+query+body）
 */
CatalogGroupHttpResult dispatchCatalogGroupRequest(const std::string& method,
                                                   const std::string& path,
                                                   const std::string& query,
                                                   const std::string& body);

}  // namespace gb

#endif
