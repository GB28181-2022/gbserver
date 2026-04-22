# GB28181 目录查询应答与仓库实现映射（2016 附录 O / 2022 附录 J）

> 本文档为工程内说明：**不引用标准正文**。合规对照请以贵方合法持有的 GB/T 28181-2016 / GB/T 28181-2022 PDF 为准。

## 1. 协议侧要点（概念层）

- **应答形态**：目录查询（Catalog）的应答体为 XML，其中设备列表通常位于 `DeviceList` 下，多条 `Item` 描述节点。
- **树形关系**：**`ParentID`** 表示父节点国标编码；**`DeviceID`** 为节点自身编码。由 `(DeviceID, ParentID)` 可构建森林；根节点父 ID 常为空或与平台约定一致。
- **常见 Item 字段**（名称因版本/厂商略有差异）：`Name`、`Manufacturer`、`Model`、`Owner`、`CivilCode`、`Address`、`Parental`（是否有子设备）、`Status`、`RegisterWay`、`Secrecy`、经纬度等；业务场景下可有 **`BusinessGroupID`** 等。
- **节点语义与 20 位编码**：设备类型由国标 ID 中类型位区分（报警、视频、行政区划、虚拟组织等）。本仓库在 [`backend/src/infra/SipCatalog.h`](../backend/src/infra/SipCatalog.h) 中归纳了 `CatalogNodeType` 与 11x/12x/13x/21x 等规则。
- **2022 附录 J**：在 2016 附录 O 对应内容基础上的修订；字段增删或约束变化需以 **2022 标准文本**核对，本仓库以**已解析并入库的字段**为准做功能对齐。

## 2. 数据库 `catalog_nodes` 映射（节选）

| 协议概念 | 表字段 | 说明 |
|----------|--------|------|
| DeviceID | `node_id` | 节点国标 ID |
| ParentID | `parent_id` | 父节点 ID（支持多级路径存储） |
| Name | `name` | 显示名 |
| 类型（设备/目录/区域） | `node_type` | 0 设备，1 目录，2 行政区域（与解析逻辑一致） |
| Parental | `parental` | 是否有子设备 |
| CivilCode | `civil_code` | 行政区划 |
| BusinessGroupID | `business_group_id` | 业务分组 |
| 子节点数等 | `item_num` / `item_index` | 与 DeviceList 条目相关 |
| 厂商型号状态等 | `manufacturer`, `model`, `status`, … | 设备属性 |

## 3. HTTP API

- `GET /api/catalog/tree?platformId=` 返回扁平 `items`，每条含 **`parentId`、`nodeId`** 及 `parental`、`businessGroupId`、`itemNum` 等，供前端组装树。详见 [`docs/api/http-contracts.md`](api/http-contracts.md) §5.0。

## 4. 厂商复合 ParentID 与前端建树顺序

- **现象**：部分下级平台/厂商下发的 `<ParentID>` 为 **`平台国标ID/父节点DeviceID`** 等多段路径，与标准「ParentID 等于父节点单一 DeviceID」字面形式不一致，但**末段通常即为父节点 ID**。
- **本仓库策略**（[`frontend/src/utils/catalogTreeGb28181.js`](../frontend/src/utils/catalogTreeGb28181.js)）按序挂边：
  1. **ParentID**：先在全部 `nodeId` 中匹配整串；失败则取 **`/` 最后一段**再匹配。
  2. **BusinessGroupID**（图 O.2 / J.2）：仍无父节点时，若 `businessGroupId` 等于某条目的 `nodeId`，挂到该业务分组节点下。
  3. **CivilCode**（图 J.1 思路）：仍为孤儿的**设备**节点，尝试父节点 `nodeId === civilCode`，或某非设备节点的 `civil_code` 字段与设备 `civilCode` 相同。
- **在线统计**：设备是否在线以 **`cameras.online`** 为准（API `cameraOnline`）；目录节点展示子树 **`(在线摄像机数/摄像机总数)`**。

## 5. 前端「实时预览」树

- 根节点展示本域信息（`gb_local_config` / `getLocalGbConfig`）。
- 子节点为接入的下级平台（`device_platforms`）。
- 平台下挂载该平台在 `catalog_nodes` 中的目录树，按上一节规则与附录示意图对齐。
