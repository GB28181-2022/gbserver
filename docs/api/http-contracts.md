## HTTP 接口契约总览（草案）

> 说明：本文件定义前后端约定的 HTTP 接口，仅包含字段与行为，不绑定具体数据库结构。

### 通用约定

- 统一响应结构：
  - 成功：`{ "code": 0, "message": "ok", "data": { ... } }`
  - 失败：`{ "code": <非 0 错误码>, "message": "错误描述", "data": null }`
- 所有时间字段统一使用 ISO8601 字符串（UTC 或带时区标识），如 `2025-02-26T12:00:00+08:00`。
- 分页参数：
  - 请求：`page`（从 1 开始），`pageSize`
  - 响应：`page`, `pageSize`, `total`

---

## 1. 认证与账号

> 对应登录页、头部“当前用户、修改密码、退出登录”等功能。

- `POST /api/auth/login`
  - 描述：用户登录，获取访问令牌。
  - 请求 `body`：
    ```json
    {
      "username": "admin",
      "password": "admin"
    }
    ```
  - 响应 `data` 示例：
    ```json
    {
      "token": "jwt-or-random-token",
      "user": {
        "username": "admin",
        "displayName": "管理员",
        "roles": ["admin"]
      }
    }
    ```

- `POST /api/auth/logout`
  - 描述：退出登录，使当前 token 失效（如有服务端状态）。
  - 请求：可为空，或携带当前 token。

- `GET /api/auth/me`
  - 描述：获取当前登录用户信息（用于刷新页面后恢复用户名显示）。
  - 响应 `data` 示例：
    ```json
    {
      "username": "admin",
      "displayName": "管理员",
      "roles": ["admin"]
    }
    ```

- `POST /api/auth/change-password`
  - 描述：修改当前登录用户密码。
  - 请求 `body`：
    ```json
    {
      "oldPassword": "admin",
      "newPassword": "Admin@2025"
    }
    ```
  - 响应：
    - 成功：`code = 0`，前端提示“修改成功”，可引导用户重新登录。
    - 失败：返回对应错误码（如旧密码不正确、密码过于简单等）。

---

## 2. 系统配置相关

### 1.1 本地国标信息

- `GET /api/config/local-gb`
  - 描述：获取本机国标编码、域、用户名、鉴权密码、通信方式等配置。
  - 响应 `data` 示例：
    ```json
    {
      "gbId": "34020000002000000001",
      "domain": "3402000000",
      "name": "本级平台",
      "username": "34020000002000000001",
      "password": "******",
      "transport": {
        "udp": true,
        "tcp": false
      }
    }
    ```

- `PUT /api/config/local-gb`
  - 描述：保存本机国标信息。
  - 请求 `body`：
    ```json
    {
      "gbId": "34020000002000000001",
      "domain": "3402000000",
      "name": "本级平台",
      "username": "34020000002000000001",
      "password": "******",
      "transport": {
        "udp": true,
        "tcp": false
      }
    }
    ```

### 1.2 流媒体配置

- `GET /api/config/media`
  - 描述：获取与 ZLMediaKit 相关的流媒体配置（**不返回** ZLM API 密钥明文）。
  - 响应 `data` 示例：
    ```json
    {
      "rtpPortRange": {
        "start": 30000,
        "end": 30500
      },
      "playbackHost": "192.168.1.9",
      "mediaApiUrl": "http://127.0.0.1:880",
      "zlmApiSecretConfigured": true,
      "rtpTransport": "udp",
      "previewInviteTimeoutSec": 45
    }
    ```
  - `playbackHost`：数据库 `media_config.media_http_host`，浏览器拉流（HTTP-FLV / WS-FLV）使用的对外主机名或 IP。
  - `mediaApiUrl`：数据库 `media_config.media_api_url`，国标服务调用 ZLMediaKit HTTP API 的根地址（完整 URL，含协议与端口）。
  - `zlmApiSecretConfigured`：数据库 `media_config.zlm_secret` 是否非空（与 ZLM `config.ini` 中 `[api] secret` 应由运维保持一致）。
  - `rtpTransport`：`udp` | `tcp`，系统默认 RTP 传输方式；对下级点播 INVITE 的 SDP 与 ZLM `openRtpServer` 的 `tcp_mode` 在未配置平台独立覆盖时采用此值。**浏览器 Web 预览播放 URL 使用 `playbackHost`，不受此字段影响。**
  - `previewInviteTimeoutSec`：库表中的秒数，供 `GET .../preview/session` 等返回「INVITING 超时阈值」等展示；默认 `45`，合法范围 `10`～`600`。**openRtpServer 长期无 RTP 时由 ZLM 回调 `POST /api/zlm/hook/on_rtp_server_timeout` 触发 gb_service 发 SIP BYE 并关会话**（须在 ZLM `config.ini` 配置该 Hook）。`on_stream_changed(regist=false)` 不负责 BYE；无人观看断流仍由 `on_stream_none_reader` 处理。

- `PUT /api/config/media`
  - 描述：保存 RTP 端口区间、播放对外主机（`playbackHost`）、ZLM HTTP API 根地址（`mediaApiUrl`）；**不接受**通过接口修改 `zlm_secret`（密钥仅运维写库或迁移脚本维护）。
  - 请求 `body` 示例：
    ```json
    {
      "start": 30000,
      "end": 30500,
      "playbackHost": "192.168.1.9",
      "mediaApiUrl": "http://127.0.0.1:880",
      "rtpTransport": "udp",
      "previewInviteTimeoutSec": 45
    }
    ```
  - `rtpTransport`：可选，缺省或非法值按 `udp` 落库。
  - `previewInviteTimeoutSec`：可选；仅当请求体包含该字段且为大于 0 的整数时更新库表，并会夹紧到 `10`～`600`。
  - 行为：成功后国标服务会以库中已有 `zlm_secret` 调用 ZLMediaKit **`/index/api/setServerConfig`**，写入 **`rtp_proxy.port_range`**（形如 `start-end`），使后续 `openRtpServer` 与页面配置一致；通常**无需重启** ZLM 进程即可对**新开的**收流生效。若修改的是 ZLM **HTTP 监听端口**等项，ZLM 侧可能仍需重启，以 ZLM 文档为准。
  - 失败：若 ZLM 不可达、鉴权失败或返回非 0，接口返回 **502**（或带说明的 4xx），且**不会**更新数据库（先同步 ZLM，成功后再写库）。

---

## 2. 平台接入（上级平台）

### 2.1 平台列表

- `GET /api/platforms`
  - 查询参数（可选）：`keyword`, `page`, `pageSize`, `includeScope`
  - 默认返回轻量列表（包含 `scopeCount`/`excludedCount`）；当 `includeScope=1` 时附带 `catalogGroupNodeIds` 与 `excludedCameraIds` 详情数组。
  - 响应 `data`：
    ```json
    {
      "items": [
        {
          "id": 1,
          "name": "上级平台 A",
          "sipDomain": "3402000000",
          "gbId": "34020000002000000001",
          "sipIp": "192.168.1.100",
          "sipPort": 5060,
          "transport": "udp",
          "regUsername": "34020000002000000001",
          "enabled": true,
          "online": true,
          "lastHeartbeatAt": null,
          "heartbeatInterval": 60,
          "scopeCount": 2,
          "excludedCount": 1
        }
      ],
      "page": 1,
      "pageSize": 10,
      "total": 1
    }
    ```

- `POST /api/platforms`
  - 描述：新增上级平台配置。
  - 请求 `body`：与 `items` 中单条结构一致（无 `id`）。
  - 成功响应 `data.id`：新建行的数据库主键，便于前端紧接着调用目录 scope。
  - 校验：`gbId` 须 20 位数字；`sipDomain`、`sipPort`、`transport`（`udp`|`tcp`）、`heartbeatInterval`（10～3600）等见后端实现。

- `PUT /api/platforms/{id}`
  - 描述：修改上级平台配置。
  - `regPassword`：**仅当** JSON 中出现 `regPassword` 且值为**非空字符串**时才更新库中密码；省略字段或传空字符串表示保留原密码。

- `PUT /api/platforms/{id}/catalog-scope`
  - 描述：保存该上级可见的编组目录范围（`catalog_group_nodes.id` 列表，子树语义由后端与 `upstream_catalog_scope` 表一致）。
  - 请求 `body`：`{ "catalogGroupNodeIds": [1,2,3], "excludedCameraIds":[13,15] }`

- `GET /api/platforms/{id}`
  - 描述：获取单个平台详情（含 `catalogGroupNodeIds`、`excludedCameraIds`）。

- `GET /api/platforms/{id}/catalog-scope`
  - 描述：仅获取该平台目录范围与排除列表，响应：
    `{"catalogGroupNodeIds":[...], "excludedCameraIds":[...]}`。

- `POST /api/platforms/{id}/catalog-notify`
  - 描述：触发向该上级主动上报一次编组目录（MESSAGE，与被动 Catalog 应答共用构建逻辑）。建议仅在 `enabled` 且注册在线时调用。

- `DELETE /api/platforms/{id}`
  - 描述：删除上级平台配置（占位，后续可改为逻辑删除）。

### 2.1.1 信令与媒体（实现说明）

- 上级 **INVITE** 点播：信令源匹配 `upstream_platforms.sip_ip`+`sip_port` 且 `enabled=true` 时，由 `UpstreamInviteBridge` 将 **Subject/To** 中的编组对外 ID 映射为下级 `device_gb_id`，`openRtpServer` → 向下级 INVITE → ZLM **`/index/api/startSendRtp`** 将 PS-RTP 发往上级 SDP 中的 `c=`/`m=` 地址；上级 **BYE/CANCEL** 结束桥接。
- 上级 **MESSAGE** `CmdType=Catalog` 查询：返回 scope 内编组目录 XML（对外 DeviceID 为编组发号 ID）。其它 CmdType 若未单独实现，可能仅回 200；与设备侧 Query/Response 全量代理可在联调中迭代。

### 2.2 平台状态（可选独立接口）

- `GET /api/platforms/{id}/status`
  - 描述：获取指定平台的在线状态及最近心跳时间。
  - 响应 `data`：
    ```json
    {
      "online": true,
      "lastHeartbeatAt": "2025-02-26T10:00:00+08:00"
    }
    ```

---

## 3. 平台管理（下级平台 / 设备树根节点）

> 与“平台管理”页面对应，更多偏向运维视角的下级平台列表和策略。

- `GET /api/device-platforms`
  - 描述：获取下级平台列表，支持名称 / ID 搜索、名单类型筛选、策略模式筛选。
  - 查询参数（可选）：`keyword`, `whitelist`, `blacklist`, `mode`, `page`, `pageSize`
  - 响应 `data.items` 示例：
    ```json
    [
      {
        "id": 1,
        "name": "下级平台一",
        "gbId": "34020000001320000001",
        "cameraCount": 128,
        "listType": "normal",
        "strategyMode": "inherit",
        "online": true,
        "customAuthPassword": "",
        "customMediaHost": "",
        "customMediaPort": 0,
        "streamMediaUrl": "",
        "streamRtpTransport": null,
        "contactIp": "192.168.1.100",
        "contactPort": 5060,
        "signalSrcIp": "203.0.113.10",
        "signalSrcPort": 32108,
        "createdAt": "2025-02-26T10:00:00Z",
        "lastHeartbeatAt": "2025-02-26T10:05:00Z"
      }
    ]
    ```
  - 字段说明：`contactIp`/`contactPort` 为设备 Contact 声明；`signalSrcIp`/`signalSrcPort` 为服务器看到的对端信令出口（NAT 映射），用于主动下发 Catalog 等；`lastHeartbeatAt` 为节流持久化的心跳时间（非每条 MESSAGE 写库），UTC ISO8601。
  - `streamRtpTransport`：仅当 `strategyMode` 为 `custom` 时有效；`udp` / `tcp` 表示覆盖系统 `media_config.rtp_transport`；`null` 表示独立配置下仍**跟随系统**默认传输方式。`strategyMode` 为 `inherit` 时忽略库中该列。
  - `POST /api/device-platforms`、`PUT /api/device-platforms/{id}` 可在 `body` 中携带 `streamRtpTransport`（`"udp"`、`"tcp"` 或 JSON `null`）；`inherit` 模式下后端会将该列置空。

- `GET /api/device-platforms/{id}/strategy`
  - 描述：获取该平台的接入策略（独立流媒体地址、鉴权密码等）。

- `PUT /api/device-platforms/{id}/strategy`
  - 描述：保存该平台的接入策略。

---

## 4. 摄像头管理

> **路径与列表主键约定**：`cameras` 表库内主键为 **BIGSERIAL** `id`。列表项 **`id`** 即该主键（JSON 中为字符串，如 `"42"`）。**国标通道编码**在字段 **`deviceGbId`**。业务唯一性为 **`(platformGbId, deviceGbId)`**（与 `device_platforms.gb_id` + 下级 DeviceID 一致）。  
> 凡 **`/api/cameras/{cameraId}/...`** 路径中的 `cameraId`：优先按 **库内 `id`** 解析（实现上为「全由数字组成且长度 ≤12」则按 BIGINT 查表）；否则按 **`device_gb_id`** 解析，此时若多平台存在相同国标通道号，须在 **query 或 JSON body** 中带 **`platformGbId`**（或 `platformDbId` / `platformId` 指向下级平台库表主键）以消歧。

- `GET /api/cameras`
  - 描述：分页获取摄像头列表，支持按名称 / 库内 id / 国标通道号 / 所属平台检索与过滤。
  - 查询参数（可选）：`keyword`, `platformKeyword`, `online`, `page`, `pageSize`
  - 响应 `data.items` 单条字段（节选）：
    | 字段 | 说明 |
    |------|------|
    | `id` | **库内主键**，预览/回放/删除等路径参数应使用该值 |
    | `deviceGbId` | 国标通道/摄像机编码（原主键语义） |
    | `platformId` | 历史兼容字段：值为**下级平台国标 ID 字符串**（与 `device_platforms.gb_id` 一致），非库表 `platform_id` |
    | `platformDbId` | 下级平台库表主键（数字），便于与 `platformId` 查询参数互转 |
    | `platformName`, `online`, `manufacturer`, … | 与实现一致 |
  - 响应 `data.items` 示例：
    ```json
    [
      {
        "id": "42",
        "deviceGbId": "34020000001320000001",
        "name": "教学楼一层-东走廊",
        "platformId": "34020000002000000001",
        "platformDbId": 1,
        "platformName": "下级平台一",
        "online": true
      }
    ]
    ```

- `DELETE /api/cameras/{cameraId}`
  - 描述：删除指定摄像头（`cameras` 表记录）。删除前会关闭该摄像头相关的本地点播会话（SIP BYE + ZLM）；并同步删除 `catalog_nodes` 中对应的设备节点（`node_type=0`，优先按 `cameras.node_ref`，否则按 `platform_id` + **`device_gb_id`**）。`catalog_node_cameras` 等外键级联由数据库处理。若设备仍在下级平台目录中，后续 Catalog 同步可能再次出现该设备。
  - 响应：`{"code":0,"message":"ok","data":null}`；不存在时 `404`。

- `POST /api/cameras/batch-delete`
  - 描述：批量删除摄像头，语义与单条删除相同；单次最多 100 条。
  - 请求 `body`：`cameraIds` 为 **库内主键**字符串数组（与列表 `id` 一致）。
    ```json
    {
      "cameraIds": ["42", "43"]
    }
    ```
  - 响应 `data`：`{"deleted":2,"notFound":0}`（`notFound` 为列表中在库中不存在的数量）。

---

## 5. 目录与编组

### 5.0 平台国标目录树（`catalog_nodes` 下级同步快照）

- `GET /api/catalog/tree?platformId={dbId}`
  - 描述：返回指定下级平台在 `catalog_nodes` 中的**扁平**节点列表；前端按 GB28181 附录规则解析 `parentId`（含厂商复合 `平台ID/父ID` 取末段）、`businessGroupId`、`CivilCode` 等组装树。设备行 **`cameraOnline`** 来自同平台 `cameras.online`。
  - 响应 `data` 示例：
    ```json
    {
      "items": [
        {
          "nodeId": "34020000002000000001",
          "parentId": null,
          "name": "根",
          "type": 1,
          "typeName": "directory",
          "manufacturer": "",
          "model": "",
          "status": "",
          "civilCode": "",
          "parental": 0,
          "businessGroupId": null,
          "itemNum": null,
          "cameraOnline": false
        }
      ],
      "total": 1
    }
    ```
  - 字段说明：`type` 0=设备、1=目录/虚拟组织、2=行政区域；`cameraOnline` 仅 `type=0` 时与 `cameras` 表一致，其余为 `false`；`parentId` 为 XML 原始值，前端建树时需规范化（见 `docs/catalog-appendix-o-j.md`）。

### 5.1 本机目录编组（`catalog_group_*`，与 `catalog_nodes` 分离）

> **双 ID**：编组树对外 **DeviceID** 使用 `gb_device_id`（目录节点）与 `catalog_gb_device_id`（挂载通道）；**不修改** `cameras.device_gb_id`（注册/点播真源）。编码规则见 `docs/catalog-group-encoding.md`。

- `GET /api/catalog-group/nodes?nested=1|0`
  - 描述：本机编组树。`nested=1`（默认）返回嵌套 `children`；`nested=0` 返回扁平 `items`（每行含 `parentId`，无 `children`）。无根时服务端会按 `gb_local_config.gb_id` 自动建根。
  - 单节点字段：`id`、`parentId`、`gbDeviceId`、`name`、`nodeType`（0=通道占位 1=虚拟组织/发号 216 2=行政区域 3=业务分组/发号 215）、`civilCode`、`businessGroupId`、`sortOrder`、`sourcePlatformId`、`sourceGbDeviceId`。

- `POST /api/catalog-group/nodes`
  - 描述：新增节点。`parentId` 为 `null` 时仅当尚不存在根节点时可创建根；否则须指定父节点 id。
  - Body：`name` 等；`nodeType` 取值 **0/1/2/3**（通道占位 / 虚拟组织·216 / 行政区域 / 业务分组·215，发号规则见 `docs/catalog-group-encoding.md`）；`gbDeviceId` 可选；`sourcePlatformId`、`sourceGbDeviceId` 用于导入溯源。

- `PUT /api/catalog-group/nodes/{id}`
  - 描述：更新 `name`、`sortOrder`、`civilCode`、`businessGroupId`（MVP 不改 `gb_device_id`）。

- `DELETE /api/catalog-group/nodes/{id}`
  - 描述：删除节点；子节点与挂载行随 FK `ON DELETE CASCADE` 删除。

- `GET /api/catalog-group/nodes/{id}/cameras`
  - 描述：该编组节点下挂载列表。每项含 **`catalogGbDeviceId`**、**`deviceGbId`**（`cameras.device_gb_id`）、**`online`**（与 `cameras.online` 同源）、`platformName`、`platformGbId`、`platformDbId`、`cameraId`。
  - 查询参数（可选）：`page`、`pageSize`（默认 `page=1,pageSize=20`，上限 200）。
  - 响应额外字段：`page`、`pageSize`、`total`。

- `PUT /api/catalog-group/nodes/{id}/cameras`
  - 描述：**全量覆盖**挂载。`cameraIds` 为 `cameras.id`（BIGINT）字符串数组；**新增**行由服务端分配 `catalog_gb_device_id`；同一 `camera_id` 仅能出现在一条挂载记录，否则 **HTTP 409**。
  - Body：`{"cameraIds":["1","2"]}`

- `GET /api/catalog-group/camera-mounts`
  - 描述：返回全部编组挂载关系，供「分配摄像头」弹窗将已挂至**其他**节点的摄像机置灰并勾选。响应：`{"items":[{"cameraId":"…","groupNodeId":"…"},…]}`（`groupNodeId` 为 `catalog_group_nodes.id`）。
  - 查询参数（可选）：`groupNodeId`（按节点过滤）、`page`、`pageSize`（仅在传 `pageSize` 时分页返回并附带 `page/pageSize/total`）。

- `GET /api/catalog-group/import-occupancy?platformId={dbId}`
  - 描述：导入向导灰显用。返回 `sourceGbDeviceIds`（本机 `catalog_group_nodes` 中已占用的 `source_gb_device_id`，限定 `source_platform_id`）；`cameraIds`（该平台下已出现在任意 `catalog_group_node_cameras` 的 `camera_id`）。

- `POST /api/catalog-group/import`
  - 描述：批量导入。Body：
    - `targetParentId`：本机编组父节点 id（`catalog_group_nodes.id`）。
    - `platformDbId`：下级平台库内 id（与 `source_*` 一致）。
    - `directories`：`[{ "sourceGbDeviceId","name","nodeType","parentSourceGbDeviceId"|null }]`（`nodeType` 须为 1/2/3，含义同 POST 节点；顶层的 `parentSourceGbDeviceId` 为 `null` 表示挂在 `targetParentId` 下）。
    - `mounts`：`[{ "cameraId","sourceDeviceGbId?","parentSourceGbDeviceId"|null }]`（`parentSourceGbDeviceId` 为 `null` 表示直接挂在 `targetParentId` 下）。
  - 冲突（源目录已导入或摄像机已挂载）：**HTTP 409**。

### 5.2 已废弃：`POST /api/catalog/nodes`

- 返回 **HTTP 410**，提示改用 `POST /api/catalog-group/nodes`。原实现写入字段与当前 `catalog_nodes` 表不一致，且与下级同步逻辑冲突。

### 5.3 同步表挂载 API（仍指向 `catalog_nodes`，非本机编组）

- `GET /api/catalog/nodes/{id}/cameras`、`PUT /api/catalog/nodes/{id}/cameras`：操作 **`catalog_node_cameras` ↔ `catalog_nodes`**，与 §5.1 编组挂载无关；编组页应仅使用 `/api/catalog-group/*`。

---

## 6. 录像检索与回放

> **方案 A（当前实现）**：检索与列表**不**使用 `POST /api/replay/search` + `taskId`；由 **`GET /api/replay/segments`** 在服务端内部触发一次国标 `RecordInfo` 查询，写入 `replay_tasks` / `replay_segments` 后返回片段列表。

### 6.1 录像片段列表（含触发检索）

- `GET /api/replay/segments`
  - 描述：在**一次请求**内完成：国标录像目录查询（若成功则写入库）、再按时间范围读取 `replay_segments` 并返回。
  - 查询参数（必填）：`cameraId`（**库内摄像头主键** `cameras.id`，与列表 `id` 一致；少数场景亦可传国标通道号并带平台消歧）、`startTime`、`endTime`（**ISO8601 带时区偏移**，与本文「通用约定」一致，如 `2025-02-26T09:00:00+08:00`）。
  - 查询参数（可选，多平台消歧，与预览接口一致）：`platformGbId`（下级平台国标 ID）；若未带 `platformGbId`，可传 `platformId`（**下级平台库表主键**数字）或 `platformDbId`（同义），服务端会解析为 `platformGbId` 再发 SIP。
  - 失败：`503`（检索超时/失败）、`400`（缺参）等；成功时 `data.items` 为数组。
  - 响应 `data.items` 单条字段：
    | 字段 | 说明 |
    |------|------|
    | `id` | 库表 `replay_segments` 主键（整数），**回放起停与下载**中的 `segmentId` 使用该值 |
    | `segmentId` | 设备/国标侧片段标识（字符串，可能为空） |
    | `startTime` / `endTime` | 字符串（服务端自库读出） |
    | `startTimeUnix` / `endTimeUnix` | 可选（整数 Unix 秒）：片段起止时间，便于前端与拖动条对齐，减少时区解析误差 |
    | `durationSeconds` | 整数秒 |
    | `downloadable` | 布尔 |

### 6.2 回放播放（Jessibuca 拉 FLV/WS-FLV）

- `POST /api/cameras/{cameraId}/replay/start`
  - 描述：对指定片段发起 **Playback** INVITE，ZLM 收流后返回与预览类似的播放地址。路径 `cameraId` 语义见本章开头「路径与列表主键约定」。
  - 请求 `body`：
    ```json
    {
      "segmentId": "123",
      "platformGbId": "34020000001180000001",
      "platformDbId": 1,
      "platformId": "1",
      "playbackStartUnix": 1730000000,
      "playbackEndUnix": 1730003600
    }
    ```
  - `segmentId`：必填，为 **`replay_segments.id`**（与 `GET .../segments` 返回的 `id` 一致）。
  - `platformGbId` / `platformDbId` / `platformId`：可选，与 `preview/start` 相同歧义消解规则。
  - **可选时间窗（与整段起播二选一扩展）**：
    - `playbackStartUnix` / `playbackEndUnix`：Unix 秒，须在库中该片段 `[t0,t1]` 内且 `end > start`；若只传 `playbackStartUnix`，结束可默认为 `t1`；服务端会夹紧并保证最短播放窗口（约 ≥10 秒），避免过短 INVITE。
    - `offsetSeconds`：相对片段 `t0` 的偏移秒数，与 `playbackStartUnix`/`playbackEndUnix` **互斥**；用于从 `t0+offset` 播到 `t1`（或等价窗口）。
  - 成功 `data`：`sessionId`、`streamId`、`replayStreamId`、`status`、`flvUrl`、`wsFlvUrl` 等。

- `POST /api/cameras/{cameraId}/replay/stop`
  - 描述：停止回放并释放会话。
  - 请求 `body`：`{ "streamId": "<与 start 返回的 streamId 一致>" }`

- `GET /api/cameras/{cameraId}/replay/session`
  - 描述：查询回放会话状态（可选轮询，与 `preview/session` 字段类似）。
  - 查询参数：`streamId`（必填）。

### 6.3 下载（服务端落盘后经 HTTP 下载）

> 由服务端在本地生成文件后，浏览器通过 **`GET /api/replay/download/{id}/file`** 拉取，**不**直连设备 HTTP 拉文件。

- `POST /api/replay/download`
  - 描述：为某片段创建异步下载任务（`replay_downloads`）。
  - 请求 `body`：`{ "segmentId", "cameraId", "platformGbId"?, "platformDbId"?, "platformId"? }`（`segmentId` 含义同回放 start；`cameraId` 为 **库内主键**或与路径规则一致的解析值）。
  - 成功 `data`：`{ "downloadId": <整数> }`

- `GET /api/replay/download/{id}`
  - 描述：查询任务状态与进度。
  - 成功 `data`：`id`、`status`（如 `processing` / `ready` / `failed`）、`progress`（0～100）、`filePath`（服务端路径，仅供排查）。

- `GET /api/replay/download/{id}/file`
  - 描述：`status=ready` 时返回录像文件附件（如 FLV）；需携带与业务接口一致的 **Authorization**。

---

## 7. 实时预览与云台控制

> 实际媒体播放由前端通过 Jessibuca 拉流，后端主要负责国标侧的信令与流地址生成。

- `POST /api/cameras/{cameraId}/preview/start`
  - 描述：发起实时预览，返回 `flvUrl`、`streamId` 等。路径 `cameraId` 语义见本章开头「路径与列表主键约定」。
  - 请求 `body`：`{ "cameraId", "platformGbId", "platformDbId", "deviceGbId"? }`（`cameraId` 与路径一致；可选 `deviceGbId` 为冗余国标通道号，服务端以库表为准）。
  - 成功时 `data` 中含 `errorMessage`（正常多为空字符串）；若长时间无 RTP，服务端可能在超时后关闭会话并置 `preview_no_rtp_after_ack`（见 `previewInviteTimeoutSec`）。

- `GET /api/cameras/{cameraId}/preview/session`
  - 描述：查询当前内存中的预览会话状态，供前端在 `inviting` 时轮询。
  - 查询参数（与 `preview/start` 歧义消解一致，可选）：`platformGbId`，或 `platformId` / `platformDbId`（数字库表主键）。
  - 无活动会话：`{"code":0,"message":"ok","data":null}`。
  - 有会话时 `data`：`sessionId`、`streamId`、`status`（`init`/`inviting`/…）、`errorMessage`、`flvUrl`、`wsFlvUrl`、`zlmPort`、`previewInviteTimeoutSec`（当前全局配置秒数）。

- `POST /api/cameras/{cameraId}/preview/stop`
  - 描述：显式停止实时预览并释放后端会话（SIP/ZLM 等）；供需要立即拆流的场景调用。
  - **实时预览墙**（`PreviewWall`）：用户关闭分屏时**不调用**本接口，仅销毁前端播放器；流由流媒体服务「无人观看存活时间」等策略回收。

- `POST /api/ptz`
  - 描述：云台控制。HTTP 入队后由 `gb_service` 在 PJSIP 工作线程向**下级平台**发送 `SIP MESSAGE`（`Application/MANSCDP+xml`，`CmdType=DeviceControl`，`PTZCmd` 为 8 字节十六进制串），信令路由与目录/点播一致（`device_platforms` 的 Contact + `signal_src` UDP 目的）。
  - 请求 `body` 字段：
    | 字段 | 必填 | 说明 |
    |------|------|------|
    | `cameraId` | 是 | **库内主键** `cameras.id`（与列表 `id` 一致）；亦可传国标通道号并带平台消歧，规则同预览路径 |
    | `command` | 是 | `up` / `down` / `left` / `right` / `zoomIn` / `zoomOut` / `irisOpen` / `irisClose` / `stop`（`stop`+`stop` 表示全部停止） |
    | `action` | 是 | `start`（开始）或 `stop`（停止该指令对应动作） |
    | `speed` | 否 | 1～3，默认 `2`，映射到国标指令字中的速度档 |
    | `platformGbId` | 否 | 平台国标 ID；与 `preview/start` 相同，多平台下建议填写 |
    | `platformDbId` / `platformId` | 否 | 与预览接口相同，用于解析 `platformGbId` |
  - 成功：`{"code":0,"message":"ok","data":null}`。
  - 失败示例：`400` 参数错误；`404` 摄像头不存在；`4001` 所属平台离线；`500` SIP 未就绪或指令无法编码。

  ```json
  {
    "cameraId": "34020000001320000001",
    "platformGbId": "34020000001180000001",
    "command": "up",
    "action": "start",
    "speed": 2
  }
  ```

