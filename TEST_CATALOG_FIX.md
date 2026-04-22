# GB28181 目录存储问题修复验证指南

## 问题概述

修复了以下三个核心问题：

1. **数据库表结构不一致** - `catalog_nodes` 表有两个版本定义，导致插入失败
2. **设备编码解析不完整** - 只支持2位类型编码，未正确处理 131/132/215/216 等3位编码
3. **错误日志不完善** - SQL错误被重定向到 `/dev/null`，无法排查问题

---

## 修复文件清单


| 文件路径                                    | 修复内容                                              |
| --------------------------------------- | ------------------------------------------------- |
| `backend/db/schema.sql`                 | 统一 `catalog_nodes` 表结构为完整字段版本                     |
| `backend/src/infra/SipCatalog.h`        | 更新 `getNodeTypeFromDeviceId()` 支持3位编码和civilcode兼容 |
| `backend/src/infra/SipCatalog.cpp`      | 更新解析逻辑，使用新的类型判断函数                                 |
| `backend/src/infra/DbUtil.cpp`          | 移除 `2>/dev/null`，捕获并记录SQL错误详情                     |
| `backend/sql/migrate_catalog_nodes.sql` | 数据库迁移脚本，修复已部署环境                                   |


---

## 部署步骤

### 步骤1：修复数据库表结构

在已部署的环境中执行迁移脚本：

```bash
# 进入 SQL 脚本目录
cd /home/user/coder/gb_service2022/backend/sql

# 执行迁移脚本
psql -U user -d gb28181 -f migrate_catalog_nodes.sql
```

**预期输出：**

```
============================================
Catalog 节点表结构迁移完成！
============================================
新表结构支持：
  - 20位国标设备编码（11x/12x/13x/21x/22x）
  - 虚拟目录(215)和虚拟分组(216)
  - 摄像机(131)、IPC(132)、NVR(121)等设备类型
  - 行政区划节点(21x)
  - 非20位编码的兼容处理（结合civil_code）
============================================
```

### 步骤2：重新编译后端服务

```bash
# 进入后端目录
cd /home/user/coder/gb_service2022/backend

# 清理并重新编译
mkdir -p build && cd build
cmake ..
make -j4

# 重启服务
sudo systemctl restart gb_service
# 或手动启动
./gb_service
```

### 步骤3：验证表结构

```bash
# 检查 catalog_nodes 表结构
psql -U user -d gb28181 -c "\d catalog_nodes"
```

**预期看到以下字段：**

```
Column          | Type                    | Description
----------------+-------------------------+-------------
id              | integer                 | 自增主键
node_id         | character varying(32)   | 节点国标ID
platform_id     | bigint                  | 平台ID
platform_gb_id  | character varying(20)   | 平台国标ID
parent_id       | character varying(32)   | 父节点ID
name            | character varying(128)  | 节点名称
node_type       | integer                 | 0=设备,1=目录,2=区域
manufacturer    | character varying(64)   | 设备厂商
model           | character varying(64)   | 设备型号
civil_code      | character varying(6)    | 行政区划码
... (其他设备字段)
```

---

## 功能验证测试

### 测试1：目录查询与存储

**操作步骤：**

1. 打开 Web 管理界面
2. 进入"设备平台"页面
3. 选择一个已注册的下级平台
4. 点击"查询目录"按钮

**预期日志输出：**

```
【CATALOG NODE】Type=设备(NVR网络硬盘录像机) ID=42000000112157000001 Name=学校门卫
【CATALOG NODE】Type=设备(网络摄像机IPC) ID=420000001132... Name=教学楼摄像头
【CATALOG NODE】Type=目录(虚拟目录) ID=42000000121517000010 Name=一楼监控组
【CATALOG PARSED】Total nodes=4 Expected=4
【CatalogSession】Saving 4 nodes to database
【CatalogSession】Saved: Devices=3 Directories=1 Regions=0
```

**数据库验证：**

```bash
psql -U user -d gb28181 -c "SELECT node_id, name, node_type FROM catalog_nodes WHERE platform_id = (SELECT id FROM device_platforms WHERE gb_id = '42000000112007000011');"
```

**预期结果：** 能查询到4条记录，设备类型正确（3个设备+1个目录）

---

### 测试2：设备类型编码解析

**测试数据示例：**


| DeviceID             | 类型编码 | 预期类型 | 预期描述       |
| -------------------- | ---- | ---- | ---------- |
| 42000000112157000001 | 121  | 设备   | NVR网络硬盘录像机 |
| 42000000113102000001 | 131  | 设备   | 摄像机        |
| 42000000113203000001 | 132  | 设备   | 网络摄像机IPC   |
| 42000000121517000010 | 215  | 目录   | 虚拟目录       |
| 42000000121617000010 | 216  | 目录   | 虚拟分组       |
| 42000000121401000001 | 214  | 区域   | 行政区划       |


**验证方法：**
在日志中搜索 `【CATALOG NODE】`，确认每个设备的类型判断正确。

---

### 测试3：错误日志输出

**测试方法：**

1. 临时断开数据库连接或制造一个错误条件
2. 触发目录查询

**预期输出（示例）：**

```
[Error] SQL execution failed (exit code=1): INSERT INTO catalog_nodes ...
[Error] PostgreSQL: ERROR: column "node_type" of relation "catalog_nodes" does not exist
```

如果看到类似上面的详细错误信息，说明错误日志功能已生效。

---

### 测试4：非20位编码兼容

**场景：** 部分老旧设备可能使用非标准的短编码

**预期行为：**

- 编码长度 < 12位：尝试通过 CivilCode 辅助判断
- 编码长度 12-19位：使用2位类型编码判断（向后兼容）
- 编码长度 >= 20位：使用3位类型编码判断（新标准）

---

## 故障排查

### 问题1：迁移脚本执行失败

**症状：** 迁移脚本报错，提示表已存在或其他冲突

**解决方案：**

```bash
# 手动删除旧表（注意：会丢失数据）
psql -U user -d gb28181 -c "DROP TABLE IF EXISTS catalog_nodes CASCADE;"

# 重新创建表
psql -U user -d gb28181 -f /home/user/coder/gb_service2022/backend/sql/create_catalog_nodes.sql
```

### 问题2：仍然无法存储目录

**症状：** 日志显示 "Failed to insert catalog node"

**排查步骤：**

1. 检查日志中的 PostgreSQL 错误详情
2. 确认表结构正确：
  ```bash
   psql -U user -d gb28181 -c "\d catalog_nodes"
  ```
3. 检查平台ID是否存在：
  ```bash
   psql -U user -d gb28181 -c "SELECT id, gb_id, name FROM device_platforms;"
  ```

### 问题3：设备类型判断错误

**症状：** 131/132 被识别为目录，或 215/216 被识别为设备

**检查方法：**
查看日志中的 `【CATALOG NODE】` 行，确认类型判断是否正确：

```
Type=设备(网络摄像机IPC)  # 正确
Type=目录(虚拟目录)        # 正确
```

如果类型判断错误，检查 `SipCatalog.h` 中的 `getNodeTypeFromDeviceId` 函数是否已更新。

---

## GB28181 设备编码速查表


| 编码  | 类型  | 说明         |
| --- | --- | ---------- |
| 111 | 目录  | 报警目录/组织节点  |
| 112 | 设备  | 报警设备       |
| 121 | 设备  | NVR网络硬盘录像机 |
| 122 | 设备  | DVR数字录像机   |
| 128 | 设备  | 视频解码器      |
| 131 | 设备  | 摄像机        |
| 132 | 设备  | 网络摄像机(IPC) |
| 133 | 设备  | 球机         |
| 134 | 设备  | 云台摄像机      |
| 215 | 目录  | 虚拟目录       |
| 216 | 目录  | 虚拟分组       |
| 21x | 区域  | 行政区划       |
| 22x | 目录  | 用户/角色相关    |


---

## 总结

修复后系统能够：

1. 正确存储目录查询结果到数据库
2. 准确识别 131/132/215/216 等3位类型编码
3. 支持非20位编码的兼容处理
4. 提供详细的SQL错误日志便于排查问题

如有其他问题，请检查日志输出或联系技术支持。