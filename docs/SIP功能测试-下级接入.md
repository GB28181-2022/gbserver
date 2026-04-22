# SIP 功能测试（下级接入 · PJSIP 方案 A）

更换为 PJSIP 实现后，请按以下步骤做一遍功能测试。

## 前置条件

- 已执行 `./scripts/build_pjproject.sh`，且 CMake 能检测到 PJSIP（编译时会打印 “PJSIP found at ... SIP will use PJSIP”）。
- PostgreSQL 已创建数据库 `gb_service2022` 并执行过 `schema.sql`。
- 后端可执行文件：`backend/build/gb_service`。

## 0. 快速脚本测试（可选）

- **SIP REGISTER**：`./scripts/test-sip-register.sh [设备ID]`（默认 34020000003000000001），预期输出「200 OK (通过)」。
- **HTTP API**：`./scripts/test-api.sh`，预期「全部通过」。

## 1. 启动服务

```bash
cd backend/build
./gb_service
```

预期日志示例：

- `gb_service HTTP on 0.0.0.0:8080`
- `gb_service SIP (PJSIP) on 0.0.0.0:5060 (Register/Keepalive)`

## 2. 测试 REGISTER（新设备自动入库并在线）

用真实下级国标平台向本机 5060 发送 REGISTER，或使用脚本/工具发送一条 REGISTER（From 为 `sip:设备ID@域`）。

**预期：**

- 平台管理（HTTP 接口或前端）中能看到该设备，且为**在线**。
- 若该设备在 `device_platforms` 中不存在，应自动 INSERT（name/gb_id 为设备 ID，list_type=normal），并设为在线。

## 3. 测试黑名单（403）

在 `device_platforms` 中将某设备 `list_type` 设为 `blacklist`，再让该设备向 5060 发送 REGISTER。

**预期：** 收到 **403 Forbidden**，且该设备不会变为在线。

## 4. 测试心跳 MESSAGE（Keepalive）

下级发送 MESSAGE，body 中包含 `<CmdType>Keepalive</CmdType>` 和 `<DeviceID>设备ID</DeviceID>`（或 From 中带设备 ID）。

**预期：**

- 响应 **200 OK**。
- 对应设备在 `device_platforms` 中 `online=true`，`updated_at` 更新。

## 5. 测试心跳超时置离线

- 停止该下级的注册/心跳（或不再发送 REGISTER/MESSAGE）。
- 等待约 **2 分钟**（后端每 60 秒执行一次：`updated_at < NOW() - 120 seconds` 置 offline）。

**预期：** 平台管理中该设备变为**离线**。

## 6. 测试 REGISTER Expires=0（注销）

对已存在的设备发送 REGISTER，Expires 头为 `0`。

**预期：** 响应 200 OK，该设备在库中 `online=false`。

---

## 可选：无 PJSIP 时的回退

若未安装 PJSIP（未执行 `build_pjproject.sh` 或已删除 `third_party/pjproject/install`），CMake 会使用 ZLToolKit UDP 的 SIP 实现（SipSession），编译时会打印 “SIP will use ZLToolKit UDP (legacy)”。上述 1～6 的测试逻辑同样适用，仅 SIP 底层由 PJSIP 换为 ZLToolKit。

## 简要检查清单

| 项           | 预期结果           |
|--------------|--------------------|
| 启动         | HTTP 8080 + SIP 5060 正常监听 |
| REGISTER 新设备 | 自动入库且在线     |
| 黑名单 REGISTER | 403 Forbidden     |
| Keepalive MESSAGE | 200 OK，online/updated_at 更新 |
| 约 2 分钟无心跳 | 设备置为离线       |
| REGISTER Expires=0 | 200 OK，设备离线   |
