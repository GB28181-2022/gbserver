/**
 * @file CatalogGroupService.cpp
 * @brief 本机目录编组：CRUD、挂载、import-occupancy、导入（去重 + 重编码）
 */
#include "infra/CatalogGroupService.h"
#include "infra/AuthHelper.h"
#include "infra/CatalogGroupEncoding.h"
#include "infra/DbUtil.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <set>
#include <vector>

namespace gb {
namespace {

std::string trimSql(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return {};
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

std::vector<std::string> splitPipeLines(const std::string& s) {
  std::vector<std::string> lines;
  std::string cur;
  for (char c : s) {
    if (c == '\n') {
      lines.push_back(trimSql(cur));
      cur.clear();
    } else
      cur += c;
  }
  lines.push_back(trimSql(cur));
  while (!lines.empty() && lines.back().empty()) lines.pop_back();
  return lines;
}

std::vector<std::string> splitCols(const std::string& line) {
  std::vector<std::string> c;
  std::string cur;
  for (char ch : line) {
    if (ch == '|') {
      c.push_back(trimSql(cur));
      cur.clear();
    } else
      cur += ch;
  }
  c.push_back(trimSql(cur));
  return c;
}

std::string jsonEsc(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '\\') o += "\\\\";
    else if (c == '"') o += "\\\"";
    else if (c == '\n' || c == '\r' || c == '\t')
      o += ' ';
    else
      o += c;
  }
  return o;
}

std::string cgJsonStr(const std::string& body, const char* key) {
  std::string prefix = std::string("\"") + key + "\":";
  size_t p = body.find(prefix);
  if (p == std::string::npos) return {};
  p += prefix.size();
  while (p < body.size() && (body[p] == ' ' || body[p] == '\t')) ++p;
  if (p >= body.size() || body[p] != '"') return {};
  ++p;
  std::string out;
  while (p < body.size()) {
    char c = body[p++];
    if (c == '"') break;
    if (c == '\\' && p < body.size()) out += body[p++];
    else
      out += c;
  }
  return out;
}

int cgJsonInt(const std::string& body, const char* key, int def = 0) {
  std::string pattern = std::string("\"") + key + "\":";
  size_t pos = body.find(pattern);
  if (pos == std::string::npos) return def;
  pos += pattern.size();
  while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) ++pos;
  if (pos >= body.size()) return def;
  return std::atoi(body.c_str() + pos);
}

bool cgJsonNullKey(const std::string& body, const char* key) {
  return body.find(std::string("\"") + key + "\":null") != std::string::npos;
}

std::string queryParam(const std::string& q, const char* key) {
  std::string k = std::string(key) + "=";
  size_t p = q.find(k);
  if (p == std::string::npos) return "";
  p += k.size();
  size_t e = q.find('&', p);
  if (e == std::string::npos) return q.substr(p);
  return q.substr(p, e - p);
}

bool isAllDigits(const std::string& s) {
  if (s.empty()) return false;
  for (char c : s)
    if (c < '0' || c > '9') return false;
  return true;
}

bool isBigintId(const std::string& s) {
  if (s.empty() || s.size() > 19) return false;
  return isAllDigits(s);
}

std::string localConfigGbRaw() {
  std::string out = gb::execPsql("SELECT COALESCE(TRIM(gb_id),'') FROM gb_local_config WHERE id=1 LIMIT 1");
  if (out.empty()) return {};
  size_t nl = out.find('\n');
  std::string line = (nl == std::string::npos) ? out : out.substr(0, nl);
  return trimSql(line);
}

CatalogGroupHttpResult err(int http, int jsonCode, const std::string& msg) {
  CatalogGroupHttpResult r;
  r.httpStatus = http;
  r.jsonBody = "{\"code\":" + std::to_string(jsonCode) + ",\"message\":\"" + jsonEsc(msg) + "\",\"data\":null}";
  return r;
}

CatalogGroupHttpResult okJson(const std::string& inner) {
  CatalogGroupHttpResult r;
  r.httpStatus = 200;
  r.jsonBody = "{\"code\":0,\"message\":\"ok\",\"data\":" + inner + "}";
  return r;
}

int maxSerialForPrefix13(const std::string& p13) {
  if (p13.size() != 13) return 0;
  std::string e = gb::escapeSqlString(p13);
  std::string sql =
      "SELECT gb_device_id FROM catalog_group_nodes WHERE char_length(gb_device_id)=20 AND gb_device_id LIKE '" +
      e + "%' "
      "UNION ALL "
      "SELECT catalog_gb_device_id FROM catalog_group_node_cameras WHERE char_length(catalog_gb_device_id)=20 AND "
      "catalog_gb_device_id LIKE '" +
      e + "%'";
  std::string out = gb::execPsql(sql.c_str());
  std::vector<std::string> ids;
  for (const std::string& line : splitPipeLines(out)) {
    if (line.size() == 20) ids.push_back(line);
  }
  return catalog_group_encoding::maxSerialForMatchingPrefix(p13, ids);
}

std::string allocateUnderParentGb(const std::string& parentGb, int nodeType) {
  if (parentGb.size() < 10) return {};
  std::string p13 = parentGb.substr(0, 10) + catalog_group_encoding::type3ForNodeType(nodeType);
  int next = maxSerialForPrefix13(p13) + 1;
  return catalog_group_encoding::childGbIdAtSerial(parentGb, nodeType, next);
}

bool gbIdGloballyFree(const std::string& id) {
  std::string e = gb::escapeSqlString(id);
  std::string q = "SELECT (SELECT COUNT(*) FROM catalog_group_nodes WHERE gb_device_id='" + e +
                  "') + (SELECT COUNT(*) FROM catalog_group_node_cameras WHERE catalog_gb_device_id='" + e + "') AS c";
  std::string out = gb::execPsql(q.c_str());
  if (out.empty()) return false;
  std::string line = out.substr(0, out.find('\n'));
  return trimSql(line) == "0";
}

bool loadNodeGb(const std::string& id, std::string& gbOut) {
  std::string q =
      "SELECT gb_device_id FROM catalog_group_nodes WHERE id=" + id + " LIMIT 1";
  std::string out = gb::execPsql(q.c_str());
  if (out.empty()) return false;
  gbOut = trimSql(out.substr(0, out.find('\n')));
  return !gbOut.empty();
}

int countRoots() {
  std::string out = gb::execPsql("SELECT COUNT(*) FROM catalog_group_nodes WHERE parent_id IS NULL");
  if (out.empty()) return 0;
  return std::atoi(trimSql(out.substr(0, out.find('\n'))).c_str());
}

CatalogGroupHttpResult ensureRootFromConfig() {
  if (countRoots() > 0) return okJson("null");
  std::string raw = localConfigGbRaw();
  std::string gb = catalog_group_encoding::normalizeGb20(raw);
  if (gb.empty()) gb = std::string(20, '0');
  if (!gbIdGloballyFree(gb)) {
    gb = allocateUnderParentGb(gb, 1);
    if (gb.empty()) return err(500, 500, "无法分配根节点国标");
  }
  std::string ins = "INSERT INTO catalog_group_nodes (parent_id, gb_device_id, name, node_type, sort_order) VALUES "
                    "(NULL,'" +
                    gb::escapeSqlString(gb) + "','本机根',1,0)";
  if (!gb::execPsqlCommand(ins)) return err(500, 500, "创建根节点失败");
  return okJson("null");
}

void skipWs(const std::string& s, size_t& p) {
  while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) ++p;
}

bool extractJsonObjectBalanced(const std::string& b, size_t start, size_t& outEnd) {
  if (start >= b.size() || b[start] != '{') return false;
  int depth = 0;
  bool inStr = false;
  bool esc = false;
  for (size_t i = start; i < b.size(); ++i) {
    char c = b[i];
    if (inStr) {
      if (esc) {
        esc = false;
        continue;
      }
      if (c == '\\') {
        esc = true;
        continue;
      }
      if (c == '"') inStr = false;
      continue;
    }
    if (c == '"') {
      inStr = true;
      continue;
    }
    if (c == '{')
      depth++;
    else if (c == '}') {
      depth--;
      if (depth == 0) {
        outEnd = i + 1;
        return true;
      }
    }
  }
  return false;
}

bool extractArrayObjects(const std::string& body, const char* key, std::vector<std::string>& objs) {
  objs.clear();
  std::string pat = std::string("\"") + key + "\":";
  size_t p = body.find(pat);
  if (p == std::string::npos) return true;
  p = body.find('[', p + pat.size());
  if (p == std::string::npos) return false;
  ++p;
  while (true) {
    skipWs(body, p);
    if (p < body.size() && body[p] == ']') return true;
    if (p >= body.size()) return false;
    if (body[p] != '{') return false;
    size_t end = 0;
    if (!extractJsonObjectBalanced(body, p, end)) return false;
    objs.push_back(body.substr(p, end - p));
    p = end;
    skipWs(body, p);
    if (p < body.size() && body[p] == ',') {
      ++p;
      continue;
    }
    if (p < body.size() && body[p] == ']') return true;
    return false;
  }
}

std::string extractNumField(const std::string& obj, const char* key) {
  std::string pat = std::string("\"") + key + "\":";
  size_t p = obj.find(pat);
  if (p == std::string::npos) return {};
  p += pat.size();
  skipWs(obj, p);
  if (p < obj.size() && obj[p] == '"') {
    ++p;
    size_t q = obj.find('"', p);
    if (q == std::string::npos) return {};
    return obj.substr(p, q - p);
  }
  size_t e = p;
  while (e < obj.size() && std::isdigit(static_cast<unsigned char>(obj[e]))) ++e;
  return obj.substr(p, e - p);
}

std::string buildNestedTree(const std::vector<std::vector<std::string>>& rows, bool nested) {
  if (rows.empty()) return nested ? "[]" : "[]";
  std::map<std::string, std::vector<size_t>> byParent;
  std::map<std::string, size_t> idIndex;
  for (size_t i = 0; i < rows.size(); ++i) {
    const auto& c = rows[i];
    if (c.size() < 10) continue;
    idIndex[c[0]] = i;
    std::string pid = c[1].empty() ? "NULL" : c[1];
    byParent[pid].push_back(i);
  }
  std::function<std::string(const std::string&)> fmtNode = [&](const std::string& idx) -> std::string {
    size_t i = idIndex[idx];
    const auto& c = rows[i];
    std::string parentJson = "null";
    if (!c[1].empty()) parentJson = c[1];
    std::string srcPlat = "null";
    if (!c[8].empty()) srcPlat = c[8];
    std::string ch = "[]";
    if (nested) {
      std::string key = c[0];
      auto it = byParent.find(key);
      if (it != byParent.end()) {
        std::string acc = "[";
        for (size_t k = 0; k < it->second.size(); ++k) {
          if (k) acc += ",";
          acc += fmtNode(rows[it->second[k]][0]);
        }
        acc += "]";
        ch = acc;
      }
    }
    std::string srcGbEsc = jsonEsc(c[9]);
    return std::string("{\"id\":") + c[0] + ",\"parentId\":" + parentJson +
           ",\"gbDeviceId\":\"" + jsonEsc(c[2]) + "\",\"name\":\"" + jsonEsc(c[3]) + "\",\"nodeType\":" + c[4] +
           ",\"civilCode\":\"" + jsonEsc(c[5]) + "\",\"businessGroupId\":\"" + jsonEsc(c[6]) + "\",\"sortOrder\":" +
           c[7] + ",\"sourcePlatformId\":" + srcPlat + ",\"sourceGbDeviceId\":\"" + srcGbEsc + "\"" +
           (nested ? std::string(",\"children\":") + ch : "") + "}";
  };
  auto itRoot = byParent.find("NULL");
  if (itRoot == byParent.end()) return "[]";
  std::string acc = "[";
  for (size_t k = 0; k < itRoot->second.size(); ++k) {
    if (k) acc += ",";
    acc += fmtNode(rows[itRoot->second[k]][0]);
  }
  acc += "]";
  return acc;
}

CatalogGroupHttpResult handleGetNodes(const std::string& query) {
  auto er = ensureRootFromConfig();
  if (er.httpStatus != 200) return er;
  (void)er;
  std::string nestedStr = queryParam(query, "nested");
  bool nested = (nestedStr != "0");
  std::string q =
      "SELECT id::text, COALESCE(parent_id::text,''), gb_device_id, name, node_type::text, "
      "COALESCE(civil_code::text,''), COALESCE(business_group_id::text,''), sort_order::text, "
      "COALESCE(source_platform_id::text,''), COALESCE(source_gb_device_id::text,'') "
      "FROM catalog_group_nodes ORDER BY parent_id NULLS FIRST, sort_order, id";
  std::string out = gb::execPsql(q.c_str());
  std::vector<std::vector<std::string>> rows;
  for (const std::string& line : splitPipeLines(out)) {
    if (line.empty()) continue;
    rows.push_back(splitCols(line));
  }
  if (!nested) {
    std::string acc = "[";
    bool first = true;
    for (const auto& c : rows) {
      if (c.size() < 10) continue;
      if (!first) acc += ",";
      first = false;
      std::string parentJson = "null";
      if (!c[1].empty()) parentJson = c[1];
      std::string srcPlat = "null";
      if (!c[8].empty()) srcPlat = c[8];
      acc += std::string("{\"id\":") + c[0] + ",\"parentId\":" + parentJson + ",\"gbDeviceId\":\"" +
             jsonEsc(c[2]) + "\",\"name\":\"" + jsonEsc(c[3]) + "\",\"nodeType\":" + c[4] + ",\"civilCode\":\"" +
             jsonEsc(c[5]) + "\",\"businessGroupId\":\"" + jsonEsc(c[6]) + "\",\"sortOrder\":" + c[7] +
             ",\"sourcePlatformId\":" + srcPlat + ",\"sourceGbDeviceId\":\"" + jsonEsc(c[9]) + "\"}";
    }
    acc += "]";
    return okJson("{\"items\":" + acc + "}");
  }
  std::string items = buildNestedTree(rows, nested);
  return okJson("{\"items\":" + items + "}");
}

CatalogGroupHttpResult handleGetNodeCameras(const std::string& idStr, const std::string& query) {
  if (!isBigintId(idStr)) return err(400, 400, "无效的节点 ID");
  int page = std::atoi(queryParam(query, "page").c_str());
  int pageSize = std::atoi(queryParam(query, "pageSize").c_str());
  if (page <= 0) page = 1;
  if (pageSize <= 0) pageSize = 20;
  if (pageSize > 200) pageSize = 200;
  int offset = (page - 1) * pageSize;
  std::string cntOut = gb::execPsql(
      ("SELECT COUNT(*)::text FROM catalog_group_node_cameras WHERE group_node_id=" + idStr).c_str());
  int total = std::atoi(trimSql(cntOut).c_str());
  std::string q =
      "SELECT m.camera_id::text, m.catalog_gb_device_id, c.device_gb_id, c.name, "
      "CASE WHEN c.online THEN 'true' ELSE 'false' END, COALESCE(p.name,''), COALESCE(p.gb_id,''), "
      "c.platform_id::text "
      "FROM catalog_group_node_cameras m JOIN cameras c ON c.id=m.camera_id "
      "JOIN device_platforms p ON p.id=c.platform_id "
      "WHERE m.group_node_id=" +
      idStr + " ORDER BY m.sort_order, m.id LIMIT " + std::to_string(pageSize) + " OFFSET " + std::to_string(offset);
  std::string out = gb::execPsql(q.c_str());
  std::string acc = "[";
  bool first = true;
  for (const std::string& line : splitPipeLines(out)) {
    if (line.empty()) continue;
    auto c = splitCols(line);
    if (c.size() < 8) continue;
    if (!first) acc += ",";
    first = false;
    acc += "{\"cameraId\":\"" + jsonEsc(c[0]) + "\",\"catalogGbDeviceId\":\"" + jsonEsc(c[1]) +
           "\",\"deviceGbId\":\"" + jsonEsc(c[2]) + "\",\"name\":\"" + jsonEsc(c[3]) + "\",\"online\":" + c[4] +
           ",\"platformName\":\"" + jsonEsc(c[5]) + "\",\"platformGbId\":\"" + jsonEsc(c[6]) +
           "\",\"platformDbId\":\"" + jsonEsc(c[7]) + "\"}";
  }
  acc += "]";
  return okJson("{\"items\":" + acc + ",\"page\":" + std::to_string(page) + ",\"pageSize\":" + std::to_string(pageSize) +
                ",\"total\":" + std::to_string(total) + "}");
}

CatalogGroupHttpResult handleGetCameraMountIndex(const std::string& query) {
  std::string groupNodeId = queryParam(query, "groupNodeId");
  int page = std::atoi(queryParam(query, "page").c_str());
  int pageSize = std::atoi(queryParam(query, "pageSize").c_str());
  if (page <= 0) page = 1;
  if (pageSize <= 0) pageSize = 0;
  if (pageSize > 500) pageSize = 500;
  int offset = pageSize > 0 ? (page - 1) * pageSize : 0;
  std::string where = "1=1";
  if (!groupNodeId.empty() && isBigintId(groupNodeId)) {
    where = "m.group_node_id=" + groupNodeId;
  }
  std::string cntOut =
      gb::execPsql(("SELECT COUNT(*)::text FROM catalog_group_node_cameras m WHERE " + where).c_str());
  int total = std::atoi(trimSql(cntOut).c_str());
  std::string q =
      "SELECT m.camera_id::text, m.group_node_id::text, m.catalog_gb_device_id, c.device_gb_id, c.name, "
      "CASE WHEN c.online THEN 'true' ELSE 'false' END, "
      "CASE WHEN p.online THEN 'true' ELSE 'false' END, "
      "COALESCE(p.name,''), COALESCE(p.gb_id,''), c.platform_id::text "
      "FROM catalog_group_node_cameras m "
      "JOIN cameras c ON c.id = m.camera_id "
      "JOIN device_platforms p ON p.id = c.platform_id "
      "WHERE " +
      where + " ORDER BY m.group_node_id, m.sort_order, m.id";
  if (pageSize > 0) {
    q += " LIMIT " + std::to_string(pageSize) + " OFFSET " + std::to_string(offset);
  }
  std::string out = gb::execPsql(q.c_str());
  std::string acc = "[";
  bool first = true;
  for (const std::string& line : splitPipeLines(out)) {
    if (line.empty()) continue;
    auto c = splitCols(line);
    if (c.size() < 10) continue;
    if (!first) acc += ",";
    first = false;
    acc += "{\"cameraId\":\"" + jsonEsc(trimSql(c[0])) + "\",\"groupNodeId\":\"" + jsonEsc(trimSql(c[1])) +
           "\",\"catalogGbDeviceId\":\"" + jsonEsc(trimSql(c[2])) +
           "\",\"deviceGbId\":\"" + jsonEsc(trimSql(c[3])) + "\",\"name\":\"" + jsonEsc(trimSql(c[4])) +
           "\",\"cameraOnline\":" + trimSql(c[5]) + ",\"platformOnline\":" + trimSql(c[6]) +
           ",\"platformName\":\"" + jsonEsc(trimSql(c[7])) + "\",\"platformGbId\":\"" + jsonEsc(trimSql(c[8])) +
           "\",\"platformDbId\":\"" + jsonEsc(trimSql(c[9])) + "\"}";
  }
  acc += "]";
  std::string meta = ",\"total\":" + std::to_string(total);
  if (pageSize > 0) {
    meta += ",\"page\":" + std::to_string(page) + ",\"pageSize\":" + std::to_string(pageSize);
  }
  return okJson("{\"items\":" + acc + meta + "}");
}

CatalogGroupHttpResult handleGetImportOccupancy(const std::string& query) {
  std::string plat = queryParam(query, "platformId");
  if (!isBigintId(plat)) return err(400, 400, "缺少或无效的 platformId");
  std::string e = gb::escapeSqlString(plat);
  std::string q1 =
      "SELECT COALESCE(source_gb_device_id,'') FROM catalog_group_nodes WHERE source_platform_id=" + e +
      " AND source_gb_device_id IS NOT NULL AND TRIM(source_gb_device_id)<>''";
  std::string o1 = gb::execPsql(q1.c_str());
  std::string q2 =
      "SELECT m.camera_id::text FROM catalog_group_node_cameras m "
      "INNER JOIN catalog_group_nodes gn ON gn.id=m.group_node_id "
      "JOIN cameras c ON c.id=m.camera_id WHERE c.platform_id=" +
      e;
  std::string o2 = gb::execPsql(q2.c_str());
  std::string arr1 = "[";
  bool f1 = true;
  for (const std::string& line : splitPipeLines(o1)) {
    if (line.empty()) continue;
    if (!f1) arr1 += ",";
    f1 = false;
    arr1 += "\"" + jsonEsc(trimSql(line)) + "\"";
  }
  arr1 += "]";
  std::string arr2 = "[";
  bool f2 = true;
  for (const std::string& line : splitPipeLines(o2)) {
    if (line.empty()) continue;
    if (!f2) arr2 += ",";
    f2 = false;
    arr2 += "\"" + jsonEsc(trimSql(line)) + "\"";
  }
  arr2 += "]";
  return okJson("{\"sourceGbDeviceIds\":" + arr1 + ",\"cameraIds\":" + arr2 + "}");
}

CatalogGroupHttpResult handlePostNode(const std::string& body) {
  std::string name = cgJsonStr(body, "name");
  if (name.empty()) return err(400, 400, "name 不能为空");
  int nodeType = cgJsonInt(body, "nodeType", 1);
  if (nodeType < 0 || nodeType > 3) return err(400, 400, "nodeType 无效");
  bool parentNull = cgJsonNullKey(body, "parentId");
  std::string parentIdStr = cgJsonStr(body, "parentId");
  if (parentIdStr.empty() && !parentNull) {
    std::string pn = extractNumField(body, "parentId");
    if (!pn.empty()) parentIdStr = pn;
  }
  std::string civil = cgJsonStr(body, "civilCode");
  std::string bg = cgJsonStr(body, "businessGroupId");
  int sortOrder = cgJsonInt(body, "sortOrder", 0);
  std::string optGb = cgJsonStr(body, "gbDeviceId");

  if (parentNull || parentIdStr.empty()) {
    if (countRoots() > 0) return err(400, 400, "根节点已存在，请指定 parentId");
    std::string gb = catalog_group_encoding::normalizeGb20(optGb);
    if (gb.empty()) gb = catalog_group_encoding::normalizeGb20(localConfigGbRaw());
    if (gb.empty()) gb = std::string(20, '0');
    if (!gbIdGloballyFree(gb)) return err(409, 409, "gbDeviceId 已占用");
    std::string ins =
        "INSERT INTO catalog_group_nodes (parent_id, gb_device_id, name, node_type, sort_order, civil_code, "
        "business_group_id) VALUES (NULL,'" +
        gb::escapeSqlString(gb) + "','" + gb::escapeSqlString(name) + "'," + std::to_string(nodeType) + "," +
        std::to_string(sortOrder) + ",'" + gb::escapeSqlString(civil) + "','" + gb::escapeSqlString(bg) +
        "') RETURNING id";
    std::string out = gb::execPsql(ins.c_str());
    if (out.empty()) return err(500, 500, "新增失败");
    std::string idLine = trimSql(out.substr(0, out.find('\n')));
    return okJson("{\"id\":" + idLine + "}");
  }
  if (!isBigintId(parentIdStr)) return err(400, 400, "无效的 parentId");
  std::string parentGb;
  if (!loadNodeGb(parentIdStr, parentGb)) return err(404, 404, "父节点不存在");
  std::string gb;
  if (!optGb.empty()) {
    gb = catalog_group_encoding::normalizeGb20(optGb);
    if (gb.size() != 20) return err(400, 400, "gbDeviceId 须为 20 位数字");
    if (!gbIdGloballyFree(gb)) return err(409, 409, "gbDeviceId 已占用");
  } else {
    gb = allocateUnderParentGb(parentGb, nodeType);
    if (gb.empty()) return err(500, 500, "分配国标失败");
  }
  std::string srcPlat = extractNumField(body, "sourcePlatformId");
  std::string srcGb = cgJsonStr(body, "sourceGbDeviceId");
  std::string srcPlatSql = srcPlat.empty() ? "NULL" : srcPlat;
  std::string srcGbSql = srcGb.empty() ? "NULL" : ("'" + gb::escapeSqlString(srcGb) + "'");
  std::string ins =
      "INSERT INTO catalog_group_nodes (parent_id, gb_device_id, name, node_type, sort_order, civil_code, "
      "business_group_id, source_platform_id, source_gb_device_id) VALUES (" +
      parentIdStr + ",'" + gb::escapeSqlString(gb) + "','" + gb::escapeSqlString(name) + "'," +
      std::to_string(nodeType) + "," + std::to_string(sortOrder) + ",'" + gb::escapeSqlString(civil) + "','" +
      gb::escapeSqlString(bg) + "'," + srcPlatSql + "," + (srcGb.empty() ? "NULL" : srcGbSql) + ") RETURNING id";
  std::string out = gb::execPsql(ins.c_str());
  if (out.empty()) return err(500, 500, "新增失败");
  std::string idLine = trimSql(out.substr(0, out.find('\n')));
  return okJson("{\"id\":" + idLine + "}");
}

CatalogGroupHttpResult handlePutNode(const std::string& idStr, const std::string& body) {
  if (!isBigintId(idStr)) return err(400, 400, "无效的节点 ID");
  std::string name = cgJsonStr(body, "name");
  if (name.empty()) return err(400, 400, "name 不能为空");
  int sortOrder = cgJsonInt(body, "sortOrder", 0);
  std::string civil = cgJsonStr(body, "civilCode");
  std::string bg = cgJsonStr(body, "businessGroupId");
  std::string sql = "UPDATE catalog_group_nodes SET name='" + gb::escapeSqlString(name) + "',sort_order=" +
                    std::to_string(sortOrder) + ",civil_code='" + gb::escapeSqlString(civil) +
                    "',business_group_id='" + gb::escapeSqlString(bg) + "',updated_at=CURRENT_TIMESTAMP WHERE id=" +
                    idStr;
  if (!gb::execPsqlCommand(sql)) return err(500, 500, "更新失败");
  return okJson("null");
}

CatalogGroupHttpResult handleDeleteNode(const std::string& idStr) {
  if (!isBigintId(idStr)) return err(400, 400, "无效的节点 ID");
  std::string sql = "DELETE FROM catalog_group_nodes WHERE id=" + idStr;
  if (!gb::execPsqlCommand(sql)) return err(500, 500, "删除失败");
  return okJson("null");
}

CatalogGroupHttpResult handlePutNodeCameras(const std::string& idStr, const std::string& body) {
  if (!isBigintId(idStr)) return err(400, 400, "无效的节点 ID");
  std::string parentGb;
  if (!loadNodeGb(idStr, parentGb)) return err(404, 404, "节点不存在");
  std::vector<std::string> ids;
  size_t p = body.find("\"cameraIds\":");
  if (p != std::string::npos) {
    p = body.find('[', p);
    if (p != std::string::npos) {
      ++p;
      while (p < body.size()) {
        while (p < body.size() && (body[p] == ' ' || body[p] == ',' || body[p] == '\t')) ++p;
        if (p >= body.size() || body[p] == ']') break;
        if (body[p] == '"') {
          ++p;
          size_t e = body.find('"', p);
          if (e == std::string::npos) break;
          ids.push_back(body.substr(p, e - p));
          p = e + 1;
        } else {
          size_t e = p;
          while (e < body.size() && std::isdigit(static_cast<unsigned char>(body[e]))) ++e;
          ids.push_back(body.substr(p, e - p));
          p = e;
        }
      }
    }
  }
  for (const std::string& cid : ids) {
    if (!isBigintId(cid)) return err(400, 400, "cameraIds 含无效项");
  }
  for (const std::string& cid : ids) {
    std::string q = "SELECT group_node_id::text FROM catalog_group_node_cameras WHERE camera_id=" + cid +
                    " AND group_node_id<>" + idStr + " LIMIT 1";
    std::string o = gb::execPsql(q.c_str());
    if (!trimSql(o.substr(0, o.find('\n'))).empty())
      return err(409, 409, "摄像头 " + cid + " 已挂载到其他编组节点");
  }
  if (!gb::execPsqlCommand("DELETE FROM catalog_group_node_cameras WHERE group_node_id=" + idStr))
    return err(500, 500, "保存失败");
  int ord = 0;
  for (const std::string& cid : ids) {
    std::string cq = "SELECT platform_id::text FROM cameras WHERE id=" + cid + " LIMIT 1";
    std::string co = gb::execPsql(cq.c_str());
    if (trimSql(co.substr(0, co.find('\n'))).empty()) return err(404, 404, "摄像头不存在: " + cid);
    std::string newGb = allocateUnderParentGb(parentGb, 0);
    if (newGb.empty()) return err(500, 500, "分配通道国标失败");
    std::string ins = "INSERT INTO catalog_group_node_cameras (group_node_id, camera_id, catalog_gb_device_id, "
                      "sort_order) VALUES (" +
                      idStr + "," + cid + ",'" + gb::escapeSqlString(newGb) + "'," + std::to_string(ord++) + ")";
    if (!gb::execPsqlCommand(ins)) return err(500, 500, "写入挂载失败");
  }
  return okJson("null");
}

CatalogGroupHttpResult handlePostImport(const std::string& body) {
  std::string targetParent = extractNumField(body, "targetParentId");
  std::string plat = extractNumField(body, "platformDbId");
  if (!isBigintId(targetParent)) return err(400, 400, "targetParentId 无效");
  if (!isBigintId(plat)) return err(400, 400, "platformDbId 无效");
  std::string parentGb;
  if (!loadNodeGb(targetParent, parentGb)) return err(404, 404, "目标父节点不存在");
  std::vector<std::string> dirObjs;
  std::vector<std::string> mountObjs;
  if (!extractArrayObjects(body, "directories", dirObjs)) return err(400, 400, "directories 格式错误");
  if (!extractArrayObjects(body, "mounts", mountObjs)) return err(400, 400, "mounts 格式错误");
  struct DirSpec {
    std::string srcGb;
    std::string name;
    int nodeType;
    std::string parentSrc;
  };
  std::vector<DirSpec> dirs;
  for (const std::string& o : dirObjs) {
    DirSpec d;
    d.srcGb = cgJsonStr(o, "sourceGbDeviceId");
    d.name = cgJsonStr(o, "name");
    d.nodeType = cgJsonInt(o, "nodeType", 1);
    if (d.srcGb.empty() || d.name.empty()) return err(400, 400, "目录项缺少 sourceGbDeviceId 或 name");
    if (d.nodeType != 1 && d.nodeType != 2 && d.nodeType != 3)
      return err(400, 400, "目录项 nodeType 须为 1=虚拟组织 2=行政区域 3=业务分组");
    if (cgJsonNullKey(o, "parentSourceGbDeviceId"))
      d.parentSrc.clear();
    else
      d.parentSrc = cgJsonStr(o, "parentSourceGbDeviceId");
    dirs.push_back(d);
  }
  for (const auto& d : dirs) {
    std::string e1 = gb::escapeSqlString(d.srcGb);
    std::string q =
        "SELECT id FROM catalog_group_nodes WHERE source_platform_id=" + plat + " AND source_gb_device_id='" + e1 +
        "' LIMIT 1";
    std::string o = gb::execPsql(q.c_str());
    if (!trimSql(o.substr(0, o.find('\n'))).empty()) return err(409, 409, "目录已导入: " + d.srcGb);
  }
  for (const std::string& o : mountObjs) {
    std::string cid = extractNumField(o, "cameraId");
    if (!isBigintId(cid)) return err(400, 400, "挂载项 cameraId 无效");
    std::string q = "SELECT id FROM catalog_group_node_cameras WHERE camera_id=" + cid + " LIMIT 1";
    std::string out = gb::execPsql(q.c_str());
    if (!trimSql(out.substr(0, out.find('\n'))).empty()) return err(409, 409, "摄像头已挂载: " + cid);
    std::string pq = "SELECT platform_id::text FROM cameras WHERE id=" + cid + " LIMIT 1";
    std::string po = gb::execPsql(pq.c_str());
    if (trimSql(po.substr(0, po.find('\n'))) != plat) return err(400, 400, "摄像头不属于指定平台: " + cid);
  }
  std::map<std::string, std::string> srcToId;
  srcToId[""] = targetParent;
  size_t remaining = dirs.size();
  const size_t dirTotal = dirs.size();
  int guard = 0;
  while (remaining > 0 && guard++ < 10000) {
    size_t before = remaining;
    for (auto it = dirs.begin(); it != dirs.end();) {
      const auto& d = *it;
      std::string pDbId;
      if (d.parentSrc.empty()) {
        pDbId = targetParent;
      } else {
        auto pit = srcToId.find(d.parentSrc);
        if (pit == srcToId.end() || pit->second.empty()) {
          ++it;
          continue;
        }
        pDbId = pit->second;
      }
      std::string pGb;
      if (!loadNodeGb(pDbId, pGb)) return err(500, 500, "父节点丢失");
      std::string newGb = allocateUnderParentGb(pGb, d.nodeType);
      if (newGb.empty()) return err(500, 500, "分配目录国标失败");
      std::string ins =
          "INSERT INTO catalog_group_nodes (parent_id, gb_device_id, name, node_type, sort_order, source_platform_id, "
          "source_gb_device_id) VALUES (" +
          pDbId + ",'" + gb::escapeSqlString(newGb) + "','" + gb::escapeSqlString(d.name) + "'," +
          std::to_string(d.nodeType) + ",0," + plat + ",'" + gb::escapeSqlString(d.srcGb) +
          "') RETURNING id";
      std::string out = gb::execPsql(ins.c_str());
      if (out.empty()) return err(500, 500, "写入目录失败");
      std::string newId = trimSql(out.substr(0, out.find('\n')));
      srcToId[d.srcGb] = newId;
      it = dirs.erase(it);
      --remaining;
    }
    if (remaining == before) return err(400, 400, "目录 parentSourceGbDeviceId 无法解析（存在环或缺父）");
  }
  for (const std::string& o : mountObjs) {
    std::string cid = extractNumField(o, "cameraId");
    std::string sdev = cgJsonStr(o, "sourceDeviceGbId");
    std::string psrc;
    if (cgJsonNullKey(o, "parentSourceGbDeviceId"))
      psrc.clear();
    else
      psrc = cgJsonStr(o, "parentSourceGbDeviceId");
    std::string mountParentId;
    if (psrc.empty()) {
      mountParentId = targetParent;
    } else {
      auto pit = srcToId.find(psrc);
      if (pit == srcToId.end() || pit->second.empty())
        return err(400, 400, "挂载父目录未创建: " + psrc);
      mountParentId = pit->second;
    }
    std::string mGb;
    if (!loadNodeGb(mountParentId, mGb)) return err(500, 500, "挂载父节点不存在");
    std::string cgb = allocateUnderParentGb(mGb, 0);
    if (cgb.empty()) return err(500, 500, "分配通道国标失败");
    std::string sdevSql = sdev.empty() ? "NULL" : ("'" + gb::escapeSqlString(sdev) + "'");
    std::string ins =
        "INSERT INTO catalog_group_node_cameras (group_node_id, camera_id, catalog_gb_device_id, sort_order, "
        "source_platform_id, source_device_gb_id) VALUES (" +
        mountParentId + "," + cid + ",'" + gb::escapeSqlString(cgb) + "',0," + plat + "," + sdevSql + ")";
    if (!gb::execPsqlCommand(ins)) return err(500, 500, "写入挂载失败");
  }
  return okJson("{\"importedDirectories\":" + std::to_string(dirTotal - dirs.size()) +
                ",\"importedMounts\":" + std::to_string(mountObjs.size()) + "}");
}

}  // namespace

CatalogGroupHttpResult dispatchCatalogGroupRequest(const std::string& method,
                                                   const std::string& path,
                                                   const std::string& query,
                                                   const std::string& body) {
  static constexpr const char kPrefix[] = "/api/catalog-group/";
  if (path.find(kPrefix) != 0) return err(404, 404, "Not found");

  if (method == "GET") {
    if (path == "/api/catalog-group/nodes") return handleGetNodes(query);
    if (path == "/api/catalog-group/camera-mounts") return handleGetCameraMountIndex(query);
    if (path == "/api/catalog-group/import-occupancy") return handleGetImportOccupancy(query);
    static constexpr const char kNodes[] = "/api/catalog-group/nodes/";
    constexpr size_t kNodesLen = sizeof(kNodes) - 1;
    static constexpr const char kCam[] = "/cameras";
    constexpr size_t kCamLen = sizeof(kCam) - 1;
    if (path.size() > kNodesLen + kCamLen && path.compare(0, kNodesLen, kNodes) == 0 &&
        path.compare(path.size() - kCamLen, kCamLen, kCam) == 0) {
      std::string id = path.substr(kNodesLen, path.size() - kNodesLen - kCamLen);
      return handleGetNodeCameras(id, query);
    }
    return err(404, 404, "Not found");
  }
  if (method == "POST") {
    if (path == "/api/catalog-group/nodes") return handlePostNode(body);
    if (path == "/api/catalog-group/import") return handlePostImport(body);
    return err(404, 404, "Not found");
  }
  if (method == "PUT") {
    static constexpr const char kNodes[] = "/api/catalog-group/nodes/";
    constexpr size_t kNodesLen = sizeof(kNodes) - 1;
    static constexpr const char kCam[] = "/cameras";
    constexpr size_t kCamLen = sizeof(kCam) - 1;
    if (path.size() > kNodesLen + kCamLen && path.compare(0, kNodesLen, kNodes) == 0 &&
        path.compare(path.size() - kCamLen, kCamLen, kCam) == 0) {
      std::string id = path.substr(kNodesLen, path.size() - kNodesLen - kCamLen);
      return handlePutNodeCameras(id, body);
    }
    if (path.size() > kNodesLen && path.compare(0, kNodesLen, kNodes) == 0) {
      std::string id = path.substr(kNodesLen);
      if (id.find('/') != std::string::npos) return err(404, 404, "Not found");
      return handlePutNode(id, body);
    }
    return err(404, 404, "Not found");
  }
  if (method == "DELETE") {
    static constexpr const char kNodes[] = "/api/catalog-group/nodes/";
    constexpr size_t kNodesLen = sizeof(kNodes) - 1;
    if (path.size() > kNodesLen && path.compare(0, kNodesLen, kNodes) == 0) {
      std::string id = path.substr(kNodesLen);
      if (id.find('/') != std::string::npos) return err(404, 404, "Not found");
      return handleDeleteNode(id);
    }
    return err(404, 404, "Not found");
  }
  return err(405, 405, "Method not allowed");
}

}  // namespace gb
