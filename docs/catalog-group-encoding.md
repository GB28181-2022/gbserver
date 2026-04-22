# 本机目录编组：GB28181 字段与 20 位编码约定

本文档与工程内 [catalog-appendix-o-j.md](catalog-appendix-o-j.md)、[backend/src/infra/SipCatalog.h](../backend/src/infra/SipCatalog.h) 对齐，作为**本机编组表** `catalog_group_*` 的编码与语义基线（合规以所持标准文本为准）。

## 1. 树与协议字段

- **DeviceID**：目录应答 `Item` 中的设备/节点国标编码；本机编组中**目录节点**用 `catalog_group_nodes.gb_device_id`，**已挂载通道**用 `catalog_group_node_cameras.catalog_gb_device_id`。
- **ParentID**：父节点的 DeviceID；由 `parent_id` 外键解析为父行的 `gb_device_id`。
- **Name**：展示名，对应协议 **Name**。
- **CivilCode**：行政类节点或设备侧行政区划，对应 `civil_code`。
- **BusinessGroupID**：业务分组国标，对应 `business_group_id`；标准中虚拟组织（216）通过该字段归属业务分组（215）。

## 2. 标准中 215 与 216（类型位第 11–13 位）

- **215 — 业务分组（Business Group）**：`ParentID` 为所属**系统编号**；表示系统下的一层业务划分。
- **216 — 虚拟组织（Virtual Organization）**：`ParentID` 为父虚拟组织；**BusinessGroupID** 为所属业务分组（215）的 DeviceID；可在业务分组下多级展开。

本机编组发号时与 `catalog_group_nodes.node_type` 对应关系见下表。

## 3. 20 位结构与 `node_type`

标准形态（与 SipCatalog 注释一致）：

`前 10 位（域/前缀）` + `3 位类型码` + `7 位序号` = 20 位数字串。

| 编组节点语义 | `catalog_group_nodes.node_type` | 分配用类型三位 |
|-------------|----------------------------------|----------------|
| 业务分组（215） | **3** | `215` |
| 虚拟组织 / 一般目录（216） | **1** | `216` |
| 行政区域 | 2 | `218`（21x 中区划类） |
| 通道占位（树节点，少用） | 0 | `131` |

说明：`node_type=0` 与同步表 `catalog_nodes` 一致表示设备类语义；本机树以目录/区划为主，**实际视频通道**一律落在 `catalog_group_node_cameras`（发号类型位 `131`）。

## 4. 双 ID 与唯一性

- **注册/点播真源**：`cameras.device_gb_id` **不修改**。
- **编组对外 DeviceID**：目录 `gb_device_id`、挂载 `catalog_gb_device_id`；二者与全库另一列**共用序号空间**（实现上按「同一 `前10+类型三位` 前缀下序号递增 + 库 UNIQUE」保证互不重复）。

## 5. 根节点

根节点 `gb_device_id` 与 `gb_local_config.gb_id` 对齐（不足 20 位时按服务内规则归一为 20 位数字）。

## 6. 历史库迁移

若库由旧版创建（`node_type` 仅 0–2 且目录一律发 215），执行 `backend/sql/migrate_catalog_group_node_type_215_216.sql`：放宽 CHECK，并将 **`gb_device_id` 第 11–13 位为 `215` 且 `node_type=1`** 的行更新为 `3`（业务分组）；已为 216 发号的行不受影响。
