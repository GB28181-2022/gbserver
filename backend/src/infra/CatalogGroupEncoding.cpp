/**
 * @file CatalogGroupEncoding.cpp
 * @brief 编组 20 位国标纯逻辑实现
 */
#include "infra/CatalogGroupEncoding.h"

#include <cstdio>
#include <cstdlib>

namespace gb {
namespace catalog_group_encoding {

std::string normalizeGb20(const std::string& raw) {
  std::string d;
  for (char c : raw) {
    if (c >= '0' && c <= '9') d += c;
  }
  if (d.size() >= 20) return d.substr(0, 20);
  if (d.empty()) return {};
  return std::string(20 - d.size(), '0') + d;
}

std::string type3ForNodeType(int nodeType) {
  if (nodeType == 2) return "218";
  if (nodeType == 0) return "131";
  if (nodeType == 3) return "215";
  return "216";
}

int maxSerialForMatchingPrefix(const std::string& prefix13, const std::vector<std::string>& candidateIds) {
  if (prefix13.size() != 13) return 0;
  int mx = 0;
  for (const std::string& line : candidateIds) {
    if (line.size() != 20) continue;
    if (line.compare(0, 13, prefix13) != 0) continue;
    int v = std::atoi(line.substr(13).c_str());
    if (v > mx) mx = v;
  }
  return mx;
}

std::string childGbIdAtSerial(const std::string& parentGb, int nodeType, int serial) {
  if (parentGb.size() < 10) return {};
  if (serial < 1 || serial > 9999999) return {};
  std::string prefix10 = parentGb.substr(0, 10);
  std::string p13 = prefix10 + type3ForNodeType(nodeType);
  char tail[8];
  std::snprintf(tail, sizeof(tail), "%07d", serial);
  return p13 + tail;
}

}  // namespace catalog_group_encoding
}  // namespace gb
