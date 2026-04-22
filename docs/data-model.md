## 核心数据模型设计（草案）

> 目标：支撑当前已定义的 HTTP 接口与前端页面，同时为后续 GB28181 能力预留扩展空间。本文件描述“逻辑表结构”，具体物理实现以 PostgreSQL 为假定目标。

---

### 1. 认证与账号

#### 1.1 `users`（用户账号）

- 用途：平台登录账号与基础权限。
- 字段示例：
  - `id` (PK, bigserial)
  - `username` (varchar, 唯一，登录名，如 `admin`)
  - `password_hash` (varchar, 加盐后的密码哈希)
  - `display_name` (varchar，可选显示名，如“管理员”)
  - `roles` (varchar 或 jsonb，简单场景可存逗号分隔角色，如 `admin`)
  - `created_at` (timestamptz)
  - `updated_at` (timestamptz)

#### 1.2 `user_tokens`（可选：登录令牌 / 会话）

- 用途：如果需要在服务端管理 token 黑名单或多终端会话。
- 字段示例：
  - `id` (PK, bigserial)
  - `user_id` (fk → users.id)
  - `token` (varchar, 存储签名后的 token 或其指纹)
  - `expired_at` (timestamptz)
  - `created_at` (timestamptz)

---

### 2. 本地国标与流媒体配置

#### 2.1 `gb_local_config`

- 用途：存储本机 GB/T 28181 相关基础信息。
- 建议单行表（id 固定为 1）。
- 字段示例：
  - `id` (PK, smallint，固定 1)
  - `gb_id` (varchar(20)) —— 本地国标 ID
  - `domain` (varchar(20)) —— 本地域
  - `name` (varchar) —— 本机名称
  - `username` (varchar) —— 注册用户名
  - `password` (varchar) —— 鉴权密码（后续可考虑加密存储）
  - `transport_udp` (boolean)
  - `transport_tcp` (boolean)
  - `updated_at` (timestamptz)

#### 2.2 `media_config`

- 用途：与 ZLMediaKit 相关的全局流媒体配置。
- 同样可采用单行表。
- 字段示例：
  - `id` (PK, smallint，固定 1)
  - `rtp_port_start` (integer)
  - `rtp_port_end` (integer)
  - `media_http_host` (varchar) —— 播放/对外媒体主机（浏览器拉流 URL）
  - `media_api_url` (varchar) —— ZLMediaKit HTTP API 根地址（完整 URL）
  - `updated_at` (timestamptz)

---

### 3. 上级平台接入（`/api/platforms`）

#### 3.1 `upstream_platforms`

- 用途：对应“平台接入”中的每一个上级 GB 平台配置。
- 字段示例：
  - `id` (PK, bigserial)
  - `name` (varchar) —— 平台名称
  - `sip_domain` (varchar(20)) —— 上级平台域
  - `gb_id` (varchar(20)) —— 上级平台国标 ID
  - `sip_ip` (varchar) —— 上级信令 IP / 域名
  - `sip_port` (integer) —— 上级信令端口
  - `transport` (varchar(8)) —— `udp` / `tcp`
  - `reg_username` (varchar) —— 注册用户名
  - `reg_password` (varchar) —— 鉴权密码
  - `enabled` (boolean) —— 是否启用
  - `heartbeat_interval` (integer) —— 心跳间隔（秒）
  - `online` (boolean) —— 当前在线状态（可由心跳/注册状态维护）
  - `last_heartbeat_at` (timestamptz, nullable)
  - `created_at` (timestamptz)
  - `updated_at` (timestamptz)

#### 3.2 `upstream_catalog_scope`

- 用途：绑定「某上级平台」与「编组目录节点」多选范围；被动/主动 Catalog 仅输出这些节点子树内的条目。
- 字段：`upstream_platform_id`（FK → `upstream_platforms.id`）、`catalog_group_node_id`（FK → `catalog_group_nodes.id`）。

#### 3.3 上级可见 DeviceID 与内部映射

- 目录 XML 中 **Item.DeviceID** 使用编组对外国标 ID（`catalog_group_nodes.gb_device_id` / 挂载通道的 `catalog_gb_device_id`），与库内 `cameras.device_gb_id` 可不同。
- 信令入口使用 `(upstream_platforms.id, 对外 DeviceID)` 解析到内部通道与下级 `device_platforms.gb_id`，用于上级 INVITE、MESSAGE 等（见 `resolveUpstreamCatalogDeviceId` 与 `UpstreamInviteBridge`）。

---

### 4. 下级平台 / 平台管理（设备平台）

#### 4.1 `device_platforms`

- 用途：“平台管理”页面中的下级平台，承载摄像头树的根节点和接入策略。
- 字段示例：
  - `id` (PK, bigserial)
  - `name` (varchar)
  - `gb_id` (varchar(20)) —— 下级平台国标 ID
  - `list_type` (varchar(16)) —— `normal` / `whitelist` / `blacklist`
  - `strategy_mode` (varchar(16)) —— `inherit` / `custom`（继承系统配置 or 独立配置）
  - `custom_media_host` (varchar, nullable) —— 独立流媒体地址
  - `custom_media_port` (integer, nullable)
  - `custom_auth_password` (varchar, nullable) —— 独立鉴权密码
  - `online` (boolean)
  - `camera_count` (integer, 可冗余存储用于统计优化)
  - `created_at` (timestamptz)
  - `updated_at` (timestamptz)

---

### 5. 摄像头管理

#### 5.1 `cameras`

- 用途：管理所有摄像头基础信息和所属平台。
- 字段示例：
  - `id` (PK, varchar(32)) —— 摄像头国标编码
  - `name` (varchar)
  - `platform_id` (bigint, fk → device_platforms.id) —— 所属下级平台
  - `platform_gb_id` (varchar(20)) —— 冗余记录平台国标 ID
  - `online` (boolean)
  - `created_at` (timestamptz)
  - `updated_at` (timestamptz)

---

### 6. 目录数据（下级同步 vs 本机编组）

#### 6.1 `catalog_nodes`（下级平台目录快照）

- 用途：SIP/HTTP 同步写入的下级 **Catalog** 扁平节点，**非**本机手工编组真源。HTTP `GET /api/catalog/tree` 只读此表。
- 关键字段（与 `backend/db/schema.sql` 一致）：`node_id`、`platform_id`、`parent_id`（协议 ParentID 原始形态）、`name`、`node_type`（0 设备 / 1 目录 / 2 行政区域）等。

#### 6.2 `catalog_node_cameras`（与 `catalog_nodes` 的挂载，遗留）

- 用途：早期将摄像头挂到 **同步目录节点** 上的关联表；与本机编组 `catalog_group_*` **无关**。

#### 6.3 `catalog_group_nodes`（本机 GB 编组树）

- 用途：Web「目录编组」左侧树真源；协议语义见 `docs/catalog-group-encoding.md`。
- 要点：`gb_device_id`（20 位目录类 DeviceID，UNIQUE）、`parent_id`（自引用）、`node_type`（0=通道占位 1=虚拟组织/216 2=行政区域 3=业务分组/215）、`civil_code`、`business_group_id`、`source_platform_id` + `source_gb_device_id`（导入溯源与去重）。

#### 6.4 `catalog_group_node_cameras`（编组挂载）

- 用途：编组节点与 `cameras` 的挂载；**编组用通道国标**为 `catalog_gb_device_id`（UNIQUE，与全部 `gb_device_id` 共用发号空间），**不修改** `cameras.device_gb_id`。
- 约束：`UNIQUE(group_node_id, camera_id)`；摄像机全局仅允许一条挂载记录（产品规则）。

---

### 7. 回放与录像任务（骨架）

> 细节将在实现“国标回看”功能时进一步完善，这里仅定义基础维度。

#### 7.1 `replay_tasks`

- 用途：一次“录像检索请求”的任务记录。
- 字段示例：
  - `id` (PK, bigserial)
  - `task_id` (varchar) —— 对外暴露的任务 ID（供接口使用）
  - `camera_id` (varchar(32), fk → cameras.id)
  - `start_time` (timestamptz)
  - `end_time` (timestamptz)
  - `status` (varchar(16)) —— `pending` / `running` / `finished` / `failed`
  - `created_at` (timestamptz)
  - `updated_at` (timestamptz)

#### 7.2 `replay_segments`

- 用途：某个任务下返回的可回放录像片段。
- 字段示例：
  - `id` (PK, bigserial)
  - `task_id` (bigint, fk → replay_tasks.id)
  - `segment_id` (varchar) —— 对外片段 ID
  - `start_time` (timestamptz)
  - `end_time` (timestamptz)
  - `duration_seconds` (integer)
  - `downloadable` (boolean)
  - `created_at` (timestamptz)

#### 7.3 `replay_downloads`

- 用途：录像下载任务记录。
- 字段示例：
  - `id` (PK, bigserial)
  - `segment_id` (bigint, fk → replay_segments.id)
  - `status` (varchar(16)) —— `pending` / `running` / `finished` / `failed`
  - `progress` (integer) —— 0–100
  - `created_at` (timestamptz)
  - `updated_at` (timestamptz)

---

### 8. 预留：GB28181 细节表（后续扩展）

> 下面这些仅作为占位章节，具体字段在实现对应能力时再细化。

#### 8.1 `sip_registrations`（注册会话）

- 用途：记录与上级 / 下级平台的 SIP 注册状态与会话信息。

#### 8.2 `catalog_subscriptions`（目录订阅）

- 用途：记录对目录订阅的请求与状态（上级/下级）。

#### 8.3 `media_sessions`（媒体会话）

- 用途：实时预览、回放时的媒体会话信息（流 ID、来源、过期时间等）。

#### 8.4 `ptz_logs`（云台操作日志，可选）

- 用途：记录重要 PTZ 控制操作，便于审计与问题排查。

