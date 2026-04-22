/**
 * @file CatalogGroupEncoding.h
 * @brief 本机目录编组 20 位国标：纯函数（可单测，不依赖数据库）
 * @details 与 SipCatalog / docs/catalog-group-encoding.md 类型位约定一致
 */
#ifndef GB_CATALOG_GROUP_ENCODING_H
#define GB_CATALOG_GROUP_ENCODING_H

#include <string>
#include <vector>

namespace gb {
namespace catalog_group_encoding {

/** @brief 仅保留数字；不足 20 位左侧补 0，超过截断为 20 位 */
std::string normalizeGb20(const std::string& raw);

/**
 * @brief 第 11–13 位类型码（与 GB28181 目录项及 CatalogGroupService 一致）
 * @param nodeType catalog_group_nodes.node_type：0=通道占位 131；1=虚拟组织 216；2=行政区域 218；3=业务分组 215
 */
std::string type3ForNodeType(int nodeType);

/**
 * @brief 在候选 ID 中取与 prefix13 匹配（前 13 位相同、全长 20）的最大末 7 位序号
 * @param prefix13 前 10 位域 + 3 位类型
 */
int maxSerialForMatchingPrefix(const std::string& prefix13, const std::vector<std::string>& candidateIds);

/**
 * @brief 在父节点国标下生成子 ID：parent 前 10 + type3(nodeType) + serial（7 位零填充）
 * @param serial 1..9999999，否则返回空串
 */
std::string childGbIdAtSerial(const std::string& parentGb, int nodeType, int serial);

}  // namespace catalog_group_encoding
}  // namespace gb

#endif
