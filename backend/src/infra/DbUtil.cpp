/**
 * @file DbUtil.cpp
 * @brief 数据库工具实现
 * @details PostgreSQL 数据库操作封装实现：
 *          - 使用 libpq 长连接执行 SQL
 *          - SELECT 查询统一返回行列文本（行 \n，列 |）
 *          - INSERT/UPDATE/DELETE 执行状态可观测
 * @date 2025
 * @note 依赖 PostgreSQL libpq，使用 '|' 分隔符、'\n' 分行
 */
#include "infra/DbUtil.h"
#include "Util/logger.h"
#include <libpq-fe.h>

#include <cctype>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

using namespace toolkit;

namespace gb {

namespace {

// 走本机 Unix Socket，保持与原 psql 默认行为一致（避免 TCP 认证策略差异）
const char* kDefaultConnInfo = "host=/var/run/postgresql dbname=gb28181 user=user";
std::mutex g_dbMu;
PGconn* g_conn = nullptr;
std::string g_connInfo;

struct DbIniConfig {
  std::string host;
  std::string port;
  std::string dbname;
  std::string user;
  std::string password;
};

std::string trim(const std::string& s) {
  size_t b = 0;
  while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) {
    ++b;
  }
  size_t e = s.size();
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
    --e;
  }
  return s.substr(b, e - b);
}

std::string quoteConnValue(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 4);
  out.push_back('\'');
  for (char c : s) {
    if (c == '\'' || c == '\\') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  out.push_back('\'');
  return out;
}

std::string buildConnInfo(const DbIniConfig& cfg) {
  std::vector<std::string> parts;
  if (!cfg.host.empty()) parts.emplace_back("host=" + quoteConnValue(cfg.host));
  if (!cfg.port.empty()) parts.emplace_back("port=" + quoteConnValue(cfg.port));
  parts.emplace_back("dbname=" + quoteConnValue(cfg.dbname));
  parts.emplace_back("user=" + quoteConnValue(cfg.user));
  if (!cfg.password.empty()) parts.emplace_back("password=" + quoteConnValue(cfg.password));
  std::ostringstream oss;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) oss << ' ';
    oss << parts[i];
  }
  return oss.str();
}

bool loadDbConfigFromIni(DbIniConfig& cfg, std::string& loadedPath, std::string& err) {
  const std::vector<std::string> candidates = {
      "config/gb_service.ini", "../config/gb_service.ini", "../../config/gb_service.ini"};
  std::ostringstream tried;
  bool firstTried = true;
  for (const auto& path : candidates) {
    if (!firstTried) tried << ", ";
    tried << path;
    firstTried = false;

    std::ifstream in(path);
    if (!in.is_open()) {
      continue;
    }
    bool parseOk = true;
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
      ++lineNo;
      std::string t = trim(line);
      if (t.empty() || t[0] == '#' || t[0] == ';') {
        continue;
      }
      size_t eq = t.find('=');
      if (eq == std::string::npos) {
        err = "invalid line " + std::to_string(lineNo) + " in " + path;
        parseOk = false;
        break;
      }
      std::string key = trim(t.substr(0, eq));
      std::string val = trim(t.substr(eq + 1));
      if (!val.empty() && (val.front() == '"' || val.front() == '\'')) {
        if (val.size() >= 2 && val.back() == val.front()) {
          val = val.substr(1, val.size() - 2);
        }
      }

      if (key == "host") {
        cfg.host = val;
      } else if (key == "port") {
        cfg.port = val;
      } else if (key == "dbname") {
        cfg.dbname = val;
      } else if (key == "user") {
        cfg.user = val;
      } else if (key == "password") {
        cfg.password = val;
      } else {
        WarnL << "【DbUtil】忽略未知数据库配置项: key=" << key << " file=" << path;
      }
    }
    if (!parseOk) {
      err += " ; tried=[" + tried.str() + "]";
      return false;
    }
    if (cfg.dbname.empty() || cfg.user.empty()) {
      err = "missing required key(dbname/user) in " + path;
      err += " ; tried=[" + tried.str() + "]";
      return false;
    }
    loadedPath = path;
    return true;
  }
  err = "config file not found ; tried=[" + tried.str() + "]";
  return false;
}

void initConnInfoLocked() {
  if (!g_connInfo.empty()) return;
  DbIniConfig cfg;
  std::string path;
  std::string err;
  if (loadDbConfigFromIni(cfg, path, err)) {
    g_connInfo = buildConnInfo(cfg);
    InfoL << "【DbUtil】使用配置文件数据库连接: file=" << path << " dbname=" << cfg.dbname
          << " user=" << cfg.user << " host=" << (cfg.host.empty() ? "(default)" : cfg.host)
          << " port=" << (cfg.port.empty() ? "(default)" : cfg.port)
          << " password=" << (cfg.password.empty() ? "not-set" : "set");
    return;
  }
  g_connInfo = kDefaultConnInfo;
  WarnL << "【DbUtil】数据库配置文件不可用，回退默认连接参数: reason=" << err
        << " default=" << kDefaultConnInfo;
}

bool ensureConnLocked() {
  initConnInfoLocked();
  if (g_conn && PQstatus(g_conn) == CONNECTION_OK) return true;
  if (g_conn) {
    PQfinish(g_conn);
    g_conn = nullptr;
  }
  g_conn = PQconnectdb(g_connInfo.c_str());
  if (!g_conn || PQstatus(g_conn) != CONNECTION_OK) {
    ErrorL << "【DbUtil】连接 PostgreSQL 失败: " << (g_conn ? PQerrorMessage(g_conn) : "null conn");
    if (g_conn) {
      PQfinish(g_conn);
      g_conn = nullptr;
    }
    return false;
  }
  return true;
}

bool execSqlLocked(const std::string& sql, PGresult*& outRes) {
  outRes = nullptr;
  if (!ensureConnLocked()) return false;
  outRes = PQexec(g_conn, sql.c_str());
  if (!outRes) {
    ErrorL << "【DbUtil】PQexec 返回空结果";
    return false;
  }
  ExecStatusType st = PQresultStatus(outRes);
  if (st == PGRES_FATAL_ERROR || st == PGRES_BAD_RESPONSE || st == PGRES_NONFATAL_ERROR) {
    ErrorL << "【DbUtil】SQL 执行失败 status=" << int(st) << " error=" << PQresultErrorMessage(outRes)
           << " sql=" << sql.substr(0, 220);
    PQclear(outRes);
    outRes = nullptr;
    return false;
  }
  return true;
}

}  // namespace

/**
 * @brief 执行 SELECT 查询
 * @param sql SQL 查询语句
 * @return 查询结果字符串
 * @details 使用 psql -AtF '|' 格式执行查询：
 *          - -A：非对齐模式（无填充空格）
 *          - -t：只输出数据行
 *          - -F '|'：使用 | 作为字段分隔符
 *          结果行用 \n 分隔，列用 | 分隔
 */
std::string execPsql(const char* sql) {
  if (!sql || !sql[0]) return {};
  std::lock_guard<std::mutex> lk(g_dbMu);
  PGresult* res = nullptr;
  if (!execSqlLocked(sql, res)) {
    if (res) PQclear(res);
    return {};
  }
  std::string out;
  if (PQresultStatus(res) == PGRES_TUPLES_OK) {
    int rows = PQntuples(res);
    int cols = PQnfields(res);
    for (int r = 0; r < rows; ++r) {
      if (r > 0) out.push_back('\n');
      for (int c = 0; c < cols; ++c) {
        if (c > 0) out.push_back('|');
        const char* v = PQgetvalue(res, r, c);
        if (v) out.append(v);
      }
    }
  }
  PQclear(res);
  return out;
}

/**
 * @brief 执行数据库命令
 * @param sql SQL 命令语句
 * @return true 执行成功
 * @details 使用 system() 调用 psql 执行 INSERT/UPDATE/DELETE
 * @note 不返回结果集，仅检查命令退出状态
 * @note 错误输出将记录到日志，便于排查问题
 */
bool execPsqlCommand(const std::string& sql) {
  if (sql.empty()) return true;
  std::lock_guard<std::mutex> lk(g_dbMu);
  PGresult* res = nullptr;
  bool ok = execSqlLocked(sql, res);
  if (!ok) {
    if (res) PQclear(res);
    return false;
  }
  ExecStatusType st = PQresultStatus(res);
  bool success = (st == PGRES_COMMAND_OK || st == PGRES_TUPLES_OK);
  if (!success) {
    ErrorL << "【execPsqlCommand】status=" << int(st) << " sql=" << sql.substr(0, 220);
  } else {
    const char* affected = PQcmdTuples(res);
    DebugL << "【execPsqlCommand】success=1 affected=" << (affected ? affected : "");
  }
  PQclear(res);
  return success;
}

void ensureUpstreamCatalogScopeTable() {
  static const char* kSql = R"SQL(
CREATE TABLE IF NOT EXISTS upstream_catalog_scope (
  id BIGSERIAL PRIMARY KEY,
  upstream_platform_id BIGINT NOT NULL REFERENCES upstream_platforms(id) ON DELETE CASCADE,
  catalog_group_node_id BIGINT NOT NULL REFERENCES catalog_group_nodes(id) ON DELETE CASCADE,
  created_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
  UNIQUE (upstream_platform_id, catalog_group_node_id)
);
CREATE INDEX IF NOT EXISTS idx_upstream_catalog_scope_upstream
  ON upstream_catalog_scope (upstream_platform_id);
)SQL";
  if (!execPsqlCommand(kSql)) {
    WarnL << "【DB】upstream_catalog_scope 未创建成功（若尚无 catalog_group_nodes 属正常）；"
             "上级编组范围与部分目录功能将不可用，执行 backend/sql/migrate_upstream_catalog_scope.sql 可修复";
  } else {
    InfoL << "【DB】已确保 upstream_catalog_scope 存在";
  }
}

void ensureUpstreamCatalogCameraExcludeTable() {
  static const char* kSql = R"SQL(
CREATE TABLE IF NOT EXISTS upstream_catalog_camera_exclude (
  upstream_platform_id BIGINT NOT NULL REFERENCES upstream_platforms(id) ON DELETE CASCADE,
  camera_id            BIGINT NOT NULL REFERENCES cameras(id) ON DELETE CASCADE,
  created_at           TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (upstream_platform_id, camera_id)
);
CREATE INDEX IF NOT EXISTS idx_upstream_cat_cam_excl_upstream
  ON upstream_catalog_camera_exclude (upstream_platform_id);
)SQL";
  if (!execPsqlCommand(kSql)) {
    WarnL << "【DB】upstream_catalog_camera_exclude 未创建成功";
  } else {
    InfoL << "【DB】已确保 upstream_catalog_camera_exclude 存在";
  }
}

}  // namespace gb
