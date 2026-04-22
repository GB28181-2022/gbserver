/**
 * @file catalog_group_encoding_test.cpp
 * @brief 编组 20 位国标纯逻辑自测（无第三方依赖，供 ctest / CI）
 */
#include "infra/CatalogGroupEncoding.h"

#include <cassert>
#include <iostream>
#include <vector>

namespace {

using gb::catalog_group_encoding::childGbIdAtSerial;
using gb::catalog_group_encoding::maxSerialForMatchingPrefix;
using gb::catalog_group_encoding::normalizeGb20;
using gb::catalog_group_encoding::type3ForNodeType;

void run_all() {
  assert(normalizeGb20("123456789012345678901234") == "12345678901234567890");
  assert(normalizeGb20("3402000000") == "00000000003402000000");
  assert(normalizeGb20("34-02-0000001320000001") == "34020000001320000001");
  assert(normalizeGb20("").empty());

  assert(type3ForNodeType(0) == "131");
  assert(type3ForNodeType(1) == "216");
  assert(type3ForNodeType(2) == "218");
  assert(type3ForNodeType(3) == "215");

  const std::string p13_215 = "3402000000215";
  std::vector<std::string> ids = {
      "34020000002150000001",
      "34020000002150000007",
      "34020000002150000003",
  };
  assert(maxSerialForMatchingPrefix(p13_215, ids) == 7);
  assert(maxSerialForMatchingPrefix("bad", ids) == 0);

  std::string parent_bg = "34020000002150000001";
  assert(childGbIdAtSerial(parent_bg, 1, 1) == "34020000002160000001");
  assert(childGbIdAtSerial(parent_bg, 1, 2) == "34020000002160000002");
  assert(childGbIdAtSerial(parent_bg, 3, 2) == "34020000002150000002");
  assert(childGbIdAtSerial(parent_bg, 0, 2) == "34020000001310000002");

  std::string parent_rootish = "34020000002000000001";
  assert(childGbIdAtSerial(parent_rootish, 3, 1) == "34020000002150000001");

  assert(childGbIdAtSerial("", 1, 1).empty());
  assert(childGbIdAtSerial(parent_bg, 1, 0).empty());
}

}  // namespace

int main() {
  try {
    run_all();
  } catch (...) {
    std::cerr << "catalog_group_encoding_test: exception\n";
    return 2;
  }
  std::cout << "catalog_group_encoding_test: OK\n";
  return 0;
}
