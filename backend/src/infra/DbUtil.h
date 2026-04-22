/**
 * @file DbUtil.h
 * @brief 数据库工具模块
 * @details 提供 PostgreSQL 数据库操作封装，供 HTTP 与 SIP 等模块共用：
 *          - 执行 SELECT 查询，返回结果集
 *          - 执行 INSERT/UPDATE/DELETE 命令
 *          - 基于 PostgreSQL libpq 长连接实现
 * @date 2025
 * @note 依赖 PostgreSQL libpq 客户端库
 */
#ifndef GB_SERVICE_DB_UTIL_H
#define GB_SERVICE_DB_UTIL_H

#include <string>

namespace gb {

/**
 * @brief 执行 SELECT 查询
 * @param sql SQL 查询语句
 * @return 查询结果，行用 \n 分隔，列用 | 分隔
 * @details 使用 psql 命令行执行查询，-AtF '|' 格式输出
 * @note 执行失败返回空字符串
 */
std::string execPsql(const char* sql);

/**
 * @brief 执行数据库命令（INSERT/UPDATE/DELETE）
 * @param sql SQL 命令语句
 * @return true 执行成功，false 执行失败
 * @details 使用 system() 调用 psql 执行命令
 * @note 不返回结果集，仅返回执行状态
 */
bool execPsqlCommand(const std::string& sql);

/**
 * @brief 启动时补全上级编组范围表（与 migrate_upstream_catalog_scope.sql 一致）
 * @note 若尚无 catalog_group_nodes 则创建失败，仅打日志
 */
void ensureUpstreamCatalogScopeTable();
void ensureUpstreamCatalogCameraExcludeTable();

}  // namespace gb

#endif
