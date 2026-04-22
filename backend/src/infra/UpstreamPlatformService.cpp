/**
 * @file UpstreamPlatformService.cpp
 * @brief 上级平台目录 XML 与对外 ID 映射
 */
#include "infra/UpstreamPlatformService.h"
#include "infra/DbUtil.h"
#include "infra/AuthHelper.h"
#include "infra/SipServerPjsip.h"
#include "Util/logger.h"
#include <sstream>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cctype>
#include <algorithm>
#include <iconv.h>
#include <cstring>

using namespace toolkit;

namespace gb {

/**
 * @brief 将 UTF-8 字符串转换为 GB2312 编码
 * @param utf8Str UTF-8 编码的输入字符串
 * @return GB2312 编码的字符串，转换失败则返回原字符串
 * @note GB28181 标准要求 XML 使用 GB2312 编码，但数据库存储为 UTF-8
 */
static std::string utf8ToGb2312(const std::string& utf8Str) {
  if (utf8Str.empty()) return utf8Str;

  iconv_t cd = iconv_open("GB2312//IGNORE", "UTF-8");
  if (cd == (iconv_t)-1) {
    WarnL << "【编码转换】iconv_open 失败，返回原字符串";
    return utf8Str;
  }

  // 预估输出缓冲区大小（GB2312 中文字符最多 2 字节，比 UTF-8 的 3 字节小）
  size_t inLen = utf8Str.size();
  size_t outLen = inLen * 2 + 4;
  std::string outBuf;
  outBuf.resize(outLen);

  char* inPtr = const_cast<char*>(utf8Str.c_str());
  char* outPtr = &outBuf[0];
  size_t inLeft = inLen;
  size_t outLeft = outLen;

  if (iconv(cd, &inPtr, &inLeft, &outPtr, &outLeft) == (size_t)-1) {
    WarnL << "【编码转换】iconv 失败: " << strerror(errno) << "，返回原字符串";
    iconv_close(cd);
    return utf8Str;
  }

  iconv_close(cd);
  outBuf.resize(outLen - outLeft);
  return outBuf;
}

std::string xmlEscapeText(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '&':
        o += "&amp;";
        break;
      case '<':
        o += "&lt;";
        break;
      case '>':
        o += "&gt;";
        break;
      case '"':
        o += "&quot;";
        break;
      default:
        o += c;
        break;
    }
  }
  return o;
}

static std::string trimField(std::string s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

static void splitLines(const std::string& out, std::vector<std::string>& lines) {
  lines.clear();
  size_t pos = 0;
  while (pos < out.size()) {
    size_t nl = out.find('\n', pos);
    std::string line = (nl == std::string::npos) ? out.substr(pos) : out.substr(pos, nl - pos);
    if (!trimField(line).empty()) lines.push_back(line);
    if (nl == std::string::npos) break;
    pos = nl + 1;
  }
}

static std::vector<std::string> splitPipe(const std::string& line) {
  std::vector<std::string> cols;
  std::string cur;
  for (char c : line) {
    if (c == '|') {
      cols.push_back(cur);
      cur.clear();
    } else
      cur += c;
  }
  cols.push_back(cur);
  return cols;
}

bool resolveUpstreamCatalogDeviceId(long long upstreamPlatformId,
                                    const std::string& catalogGbDeviceId,
                                    std::string& outDeviceGbId,
                                    std::string& outPlatformGbId,
                                    long long& outCameraDbId) {
  outDeviceGbId.clear();
  outPlatformGbId.clear();
  outCameraDbId = 0;
  if (upstreamPlatformId <= 0 || catalogGbDeviceId.empty()) return false;
  std::string esc = escapeSqlString(catalogGbDeviceId);
  std::string uid = std::to_string(upstreamPlatformId);
  // 递归展开 scope 下的子树，再匹配摄像头
  std::string sql =
      "WITH RECURSIVE sub AS ( "
      "  SELECT n.id FROM catalog_group_nodes n "
      "  WHERE n.id IN (SELECT catalog_group_node_id FROM upstream_catalog_scope WHERE upstream_platform_id = " + uid + ") "
      "  UNION ALL "
      "  SELECT c2.id FROM catalog_group_nodes c2 JOIN sub s ON c2.parent_id = s.id "
      ") "
      "SELECT c.device_gb_id, c.platform_gb_id, c.id::text "
      "FROM catalog_group_node_cameras cgnc "
      "JOIN sub ON sub.id = cgnc.group_node_id "
      "JOIN cameras c ON c.id = cgnc.camera_id "
      "WHERE cgnc.catalog_gb_device_id = '" + esc + "' "
      "AND NOT EXISTS (SELECT 1 FROM upstream_catalog_camera_exclude ex "
      "WHERE ex.upstream_platform_id = " + uid + " AND ex.camera_id = c.id) LIMIT 1";
  std::string out = execPsql(sql.c_str());
  if (out.empty()) return false;
  std::vector<std::string> lines;
  splitLines(out, lines);
  if (lines.empty()) return false;
  std::vector<std::string> cols = splitPipe(lines[0]);
  if (cols.size() < 3) return false;
  outDeviceGbId = trimField(cols[0]);
  outPlatformGbId = trimField(cols[1]);
  outCameraDbId = std::atoll(trimField(cols[2]).c_str());
  return !outDeviceGbId.empty() && !outPlatformGbId.empty();
}

static const char* nodeStatusOnOff(const std::string& onlineCol) {
  return (onlineCol == "t" || onlineCol == "true" || onlineCol == "1") ? "ON" : "OFF";
}

/**
 * @brief 生成目录节点 Item XML（Parental=1，GB28181 §A.2.1）
 */
static void appendNodeItem(std::ostringstream& xml, int index, const std::string& deviceId,
                           const std::string& name, const std::string& parentId,
                           const std::string& businessGroupId) {
  xml << "<Item Index=\"" << index << "\">\r\n";
  xml << "<DeviceID>" << xmlEscapeText(deviceId) << "</DeviceID>\r\n";
  xml << "<Name>" << xmlEscapeText(name) << "</Name>\r\n";
  xml << "<ParentID>" << xmlEscapeText(parentId) << "</ParentID>\r\n";
  if (!businessGroupId.empty())
    xml << "<BusinessGroupID>" << xmlEscapeText(businessGroupId) << "</BusinessGroupID>\r\n";
  xml << "</Item>\r\n";
}

/**
 * @brief 生成摄像头 Item XML（Parental=0，GB28181 §A.2.2）
 */
static void appendCameraItem(std::ostringstream& xml, int index, const std::string& deviceId,
                             const std::string& name, const std::string& parentId,
                             const std::string& manufacturer, const std::string& model,
                             const std::string& owner, const std::string& civilCode,
                             const std::string& address, int safetyWay, int registerWay,
                             const std::string& secrecy, const char* status,
                             const std::string& businessGroupId) {
  xml << "<Item Index=\"" << index << "\">\r\n";
  xml << "<DeviceID>" << xmlEscapeText(deviceId) << "</DeviceID>\r\n";
  xml << "<Parental>0</Parental>\r\n";
  xml << "<ParentID>" << xmlEscapeText(parentId) << "</ParentID>\r\n";
  xml << "<Name>" << xmlEscapeText(name) << "</Name>\r\n";
  xml << "<Status>" << status << "</Status>\r\n";
  xml << "<Manufacturer>" << xmlEscapeText(manufacturer) << "</Manufacturer>\r\n";
  xml << "<Model>" << xmlEscapeText(model) << "</Model>\r\n";
  xml << "<Owner>" << xmlEscapeText(owner) << "</Owner>\r\n";
  xml << "<CivilCode>" << xmlEscapeText(civilCode) << "</CivilCode>\r\n";
  xml << "<Address>" << xmlEscapeText(address) << "</Address>\r\n";
  xml << "<SafetyWay>" << safetyWay << "</SafetyWay>\r\n";
  xml << "<RegisterWay>" << registerWay << "</RegisterWay>\r\n";
  xml << "<Secrecy>" << xmlEscapeText(secrecy) << "</Secrecy>\r\n";
  xml << "<Block></Block>\r\n";
  xml << "<CertNum></CertNum>\r\n";
  xml << "<Certifiable></Certifiable>\r\n";
  xml << "<ErrCode></ErrCode>\r\n";
  xml << "<ErrTime></ErrTime>\r\n";
  if (!businessGroupId.empty())
    xml << "<BusinessGroupID>" << xmlEscapeText(businessGroupId) << "</BusinessGroupID>\r\n";
  xml << "</Item>\r\n";
}

static bool loadSubtreeRows(long long upstreamPlatformId,
                            std::vector<std::vector<std::string>>& nodeRows,
                            std::vector<std::vector<std::string>>& camRows) {
  nodeRows.clear();
  camRows.clear();
  std::string uid = std::to_string(upstreamPlatformId);
  // down_sub：scope 根整子树（候选集）。对半选目录，只应上报"有被推送摄像头的路径上的目录"，
  // 避免把 scope 根下无被推摄像头的兄弟目录也带出。
  // pushed_mount_dirs：scope 子树内，有被推送摄像头挂载的目录
  // relevant_paths：从 pushed_mount_dirs 沿 parent_id 向上（限定在 down_sub 内）到 scope 根的所有路径目录
  // relevant_down：scope 根（保证 root 自身总被上报）∪ relevant_paths（落在 down_sub 内的部分）
  // 最终 nodes_all = relevant_down ∪ up_chain
  std::string sqlNodes =
      "WITH RECURSIVE roots AS ( "
      "  SELECT DISTINCT catalog_group_node_id AS id "
      "  FROM upstream_catalog_scope WHERE upstream_platform_id = " +
      uid +
      "), "
      "down_sub AS ( "
      "  SELECT n.id, n.parent_id, n.gb_device_id, n.name, n.node_type, "
      "         COALESCE(n.civil_code,'') AS civil_code, "
      "         COALESCE(n.business_group_id,'') AS business_group_id, n.sort_order "
      "  FROM catalog_group_nodes n "
      "  WHERE n.id IN (SELECT id FROM roots) "
      "  UNION ALL "
      "  SELECT c.id, c.parent_id, c.gb_device_id, c.name, c.node_type, "
      "         COALESCE(c.civil_code,'') AS civil_code, "
      "         COALESCE(c.business_group_id,'') AS business_group_id, c.sort_order "
      "  FROM catalog_group_nodes c INNER JOIN down_sub p ON c.parent_id = p.id "
      "), "
      "pushed_mount_dirs AS ( "
      "  SELECT DISTINCT cgnc.group_node_id AS id "
      "  FROM catalog_group_node_cameras cgnc "
      "  WHERE cgnc.group_node_id IN (SELECT id FROM down_sub) "
      "    AND NOT EXISTS (SELECT 1 FROM upstream_catalog_camera_exclude ex "
      "                    WHERE ex.upstream_platform_id = " +
      uid +
      "                      AND ex.camera_id = cgnc.camera_id) "
      "), "
      "relevant_paths AS ( "
      "  SELECT id FROM pushed_mount_dirs "
      "  UNION "
      "  SELECT cn.parent_id AS id "
      "  FROM catalog_group_nodes cn "
      "  INNER JOIN relevant_paths rp ON cn.id = rp.id "
      "  WHERE cn.parent_id IS NOT NULL "
      "    AND cn.parent_id IN (SELECT id FROM down_sub) "
      "), "
      "relevant_down AS ( "
      "  SELECT id FROM roots "
      "  UNION "
      "  SELECT id FROM down_sub WHERE id IN (SELECT id FROM relevant_paths) "
      "), "
      "up_chain AS ( "
      "  SELECT n.id, n.parent_id, n.gb_device_id, n.name, n.node_type, "
      "         COALESCE(n.civil_code,'') AS civil_code, "
      "         COALESCE(n.business_group_id,'') AS business_group_id, n.sort_order "
      "  FROM catalog_group_nodes n "
      "  WHERE n.id IN (SELECT id FROM roots) "
      "  UNION ALL "
      "  SELECT p.id, p.parent_id, p.gb_device_id, p.name, p.node_type, "
      "         COALESCE(p.civil_code,'') AS civil_code, "
      "         COALESCE(p.business_group_id,'') AS business_group_id, p.sort_order "
      "  FROM catalog_group_nodes p INNER JOIN up_chain c ON c.parent_id = p.id "
      "), "
      "nodes_all AS ( "
      "  SELECT * FROM down_sub WHERE id IN (SELECT id FROM relevant_down) "
      "  UNION "
      "  SELECT * FROM up_chain "
      ") "
      "SELECT s.id::text, COALESCE(pg.gb_device_id,''), s.gb_device_id, s.name, s.node_type::text, "
      "       s.civil_code, s.business_group_id "
      "FROM nodes_all s "
      "LEFT JOIN catalog_group_nodes pg ON pg.id = s.parent_id "
      "ORDER BY s.parent_id NULLS FIRST, s.sort_order, s.id";
  std::string outN = execPsql(sqlNodes.c_str());
  std::vector<std::string> lines;
  splitLines(outN, lines);
  for (const auto& ln : lines) {
    std::vector<std::string> c = splitPipe(ln);
    if (c.size() >= 7) nodeRows.push_back(std::move(c));
  }

  // GB28181 §附录D/O：自动推导 BusinessGroupID
  // col[0]=id, col[1]=parentGb, col[2]=selfGb, col[6]=businessGroupId
  // type_code = gb_device_id 第11-13位：215=业务分组, 216=虚拟组织
  // 规则：业务分组(215)自身不填 BusinessGroupID，其余节点向上遍历找最近的215祖先
  {
    // 建立 gbDeviceId → row index 映射 和 gbDeviceId → parentGb 映射
    std::unordered_map<std::string, size_t> gbToIdx;
    for (size_t i = 0; i < nodeRows.size(); ++i) {
      gbToIdx[nodeRows[i][2]] = i;
    }
    for (size_t i = 0; i < nodeRows.size(); ++i) {
      auto& row = nodeRows[i];
      const std::string& gb = row[2];
      // 已有值则跳过
      if (!row[6].empty()) continue;
      // 业务分组(215)自身不填
      if (gb.size() >= 13 && gb.substr(10, 3) == "215") continue;
      // 向上遍历找215祖先
      std::string cur = row[1];  // 从 parentGb 开始
      std::unordered_set<std::string> visited;
      while (!cur.empty() && visited.find(cur) == visited.end()) {
        visited.insert(cur);
        if (cur.size() >= 13 && cur.substr(10, 3) == "215") {
          row[6] = cur;  // 找到业务分组
          break;
        }
        auto it = gbToIdx.find(cur);
        if (it == gbToIdx.end()) break;
        cur = nodeRows[it->second][1];  // 继续向上
      }
    }
  }

  std::string sqlCam =
      "WITH RECURSIVE sub AS ( "
      "SELECT n.id FROM catalog_group_nodes n "
      "WHERE n.id IN (SELECT catalog_group_node_id FROM upstream_catalog_scope WHERE upstream_platform_id = " +
      uid +
      ") "
      "UNION ALL "
      "SELECT c.id FROM catalog_group_nodes c INNER JOIN sub p ON c.parent_id = p.id "
      ") "
      "SELECT cgnc.catalog_gb_device_id, gn.gb_device_id, c.name, "
      "COALESCE(c.manufacturer,''), COALESCE(c.model,''), COALESCE(c.owner,''), COALESCE(c.civil_code,''), "
      "CASE WHEN c.online THEN 't' ELSE 'f' END, "
      "COALESCE(c.address,''), COALESCE(c.safety_way,0), COALESCE(c.register_way,1), "
      "COALESCE(c.secrecy,'0') "
      "FROM catalog_group_node_cameras cgnc "
      "JOIN sub ON sub.id = cgnc.group_node_id "
      "JOIN catalog_group_nodes gn ON gn.id = cgnc.group_node_id "
      "JOIN cameras c ON c.id = cgnc.camera_id "
      "WHERE NOT EXISTS (SELECT 1 FROM upstream_catalog_camera_exclude ex "
      "WHERE ex.upstream_platform_id = " +
      uid + " AND ex.camera_id = c.id) "
      "ORDER BY cgnc.sort_order, cgnc.id";
  std::string outC = execPsql(sqlCam.c_str());
  lines.clear();
  splitLines(outC, lines);
  for (const auto& ln : lines) {
    std::vector<std::string> c = splitPipe(ln);
    if (c.size() >= 12) camRows.push_back(std::move(c));
  }
  return true;
}

bool buildUpstreamCatalogResponseXml(long long upstreamPlatformId,
                                     const std::string& queryDeviceId,
                                     int sn,
                                     std::string& outXml) {
  std::vector<std::vector<std::string>> nodeRows, camRows;
  loadSubtreeRows(upstreamPlatformId, nodeRows, camRows);

  // 建立 nodeGbDeviceId → BusinessGroupID 映射，用于摄像头 Item 查找所属业务分组
  std::unordered_map<std::string, std::string> nodeBizMap;
  for (const auto& r : nodeRows) {
    nodeBizMap[r[2]] = r[6];
  }

  std::ostringstream items;
  int itemCount = 0;

  for (const auto& row : nodeRows) {
    appendNodeItem(items, itemCount, row[2], row[3].empty() ? row[2] : row[3], row[1], row[6]);
    ++itemCount;
  }
  for (const auto& row : camRows) {
    // 摄像头的 BusinessGroupID = 其父节点(row[1])的 BusinessGroupID
    std::string camBiz;
    auto it = nodeBizMap.find(row[1]);
    if (it != nodeBizMap.end()) camBiz = it->second;
    appendCameraItem(items, itemCount, row[0], row[2].empty() ? row[0] : row[2], row[1],
                     row[3], row[4], row[5], row[6], row[8],
                     std::atoi(row[9].c_str()), std::atoi(row[10].c_str()), row[11],
                     nodeStatusOnOff(row[7]), camBiz);
    ++itemCount;
  }

  std::ostringstream xml;
  xml << "<?xml version=\"1.0\" encoding=\"GB2312\"?>\r\n";
  xml << "<Response>\r\n";
  xml << "<CmdType>Catalog</CmdType>\r\n";
  xml << "<SN>" << sn << "</SN>\r\n";
  xml << "<DeviceID>" << xmlEscapeText(queryDeviceId.empty() ? getSystemGbId() : queryDeviceId)
      << "</DeviceID>\r\n";
  xml << "<SumNum>" << itemCount << "</SumNum>\r\n";
  xml << "<DeviceList Num=\"" << itemCount << "\">\r\n";
  xml << items.str();
  xml << "</DeviceList>\r\n";
  xml << "</Response>\r\n";
  outXml = utf8ToGb2312(xml.str());
  return true;
}


bool buildUpstreamCatalogNotifyXmlParts(long long upstreamPlatformId, int& ioSn, std::vector<std::string>& outParts) {
  outParts.clear();
  std::vector<std::vector<std::string>> nodeRows, camRows;
  loadSubtreeRows(upstreamPlatformId, nodeRows, camRows);

  // 建立 nodeGbDeviceId → BusinessGroupID 映射
  std::unordered_map<std::string, std::string> nodeBizMap;
  for (const auto& r : nodeRows) {
    nodeBizMap[r[2]] = r[6];
  }

  struct Item {
    std::string xml;
    size_t len;
  };
  std::vector<Item> items;
  items.reserve(nodeRows.size() + camRows.size());
  int globalIdx = 0;

  for (const auto& row : nodeRows) {
    std::ostringstream one;
    appendNodeItem(one, globalIdx++, row[2], row[3].empty() ? row[2] : row[3], row[1], row[6]);
    std::string s = one.str();
    size_t len = s.size();
    items.push_back({std::move(s), len});
  }
  for (const auto& row : camRows) {
    std::ostringstream one;
    std::string camBiz;
    auto it = nodeBizMap.find(row[1]);
    if (it != nodeBizMap.end()) camBiz = it->second;
    appendCameraItem(one, globalIdx++, row[0], row[2].empty() ? row[0] : row[2], row[1],
                     row[3], row[4], row[5], row[6], row[8],
                     std::atoi(row[9].c_str()), std::atoi(row[10].c_str()), row[11],
                     nodeStatusOnOff(row[7]), camBiz);
    std::string s = one.str();
    size_t len = s.size();
    items.push_back({std::move(s), len});
  }

  const int total = static_cast<int>(items.size());
  std::string localId = getSystemGbId();
  if (localId.empty()) localId = "34020000002000000001";
  const std::string escLocalId = xmlEscapeText(localId);

  auto emitOne = [&](int sn, int sumNum, int begin, int count, size_t itemsBytes) {
    (void)itemsBytes;
    std::ostringstream xml;
    xml << "<?xml version=\"1.0\" encoding=\"GB2312\"?>\r\n";
    xml << "<Response>\r\n";
    xml << "<CmdType>Catalog</CmdType>\r\n";
    xml << "<SN>" << sn << "</SN>\r\n";
    xml << "<DeviceID>" << escLocalId << "</DeviceID>\r\n";
    xml << "<SumNum>" << sumNum << "</SumNum>\r\n";
    xml << "<DeviceList Num=\"" << count << "\">\r\n";
    for (int i = 0; i < count; ++i) xml << items[begin + i].xml;
    xml << "</DeviceList>\r\n";
    xml << "</Response>\r\n";
    outParts.push_back(utf8ToGb2312(xml.str()));
  };

  if (total == 0) {
    emitOne(ioSn++, 0, 0, 0, 0);
    InfoL << "【上级目录上报】upstreamId=" << upstreamPlatformId << " 目录项=0 摄像机项=0 合计=0 分包=1";
    return true;
  }

  // GB28181 参考实现：每包 1 个 Item（<DeviceList Num="1">），与合规平台行为一致
  for (int i = 0; i < total; ++i) {
    emitOne(ioSn++, total, i, 1, items[i].len);
  }

  InfoL << "【上级目录上报】upstreamId=" << upstreamPlatformId << " 目录项=" << nodeRows.size()
        << " 摄像机项=" << camRows.size() << " 合计=" << total << " 分包=" << total
        << " (每包1条)";
  return true;
}

}  // namespace gb
