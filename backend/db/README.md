# 数据库骨架与迁移

## 全量初始化

新环境可直接执行仓库内骨架（需已安装 PostgreSQL 客户端）：

```bash
psql "$DATABASE_URL" -v ON_ERROR_STOP=1 -f backend/db/schema.sql
```

## 已有库增量升级

按特性执行 `backend/sql/` 下对应脚本，**建议先备份**。

### 本机目录编组 `catalog_group_*`

- **脚本**: `backend/sql/migrate_add_catalog_group.sql`
- **顺序**: 在 `cameras`、`device_platforms` 已存在之后执行（外键依赖）。
- **幂等**: `CREATE TABLE IF NOT EXISTS` / `CREATE INDEX IF NOT EXISTS`，可重复执行。
- **回滚**: 删除表会级联丢失编组数据；`catalog_gb_device_id` 已发号不做回收，删除前请评估。

```bash
psql "$DATABASE_URL" -v ON_ERROR_STOP=1 -f backend/sql/migrate_add_catalog_group.sql
```

迁移完成后启动 `gb_service`（见 `.cursor/rules/dev-build-run.mdc`）。

### 编组节点类型 215 / 216（node_type 扩展）

- **脚本**: `backend/sql/migrate_catalog_group_node_type_215_216.sql`
- **作用**: `node_type` 允许 `3`（业务分组，国标类型位 215）；原目录行 `node_type=1` 且历史发号为 215 的更新为 `3`；此后新建「虚拟组织/目录」为 `node_type=1`（类型位 216）。详见 [catalog-group-encoding.md](../../docs/catalog-group-encoding.md)。

```bash
psql "$DATABASE_URL" -v ON_ERROR_STOP=1 -f backend/sql/migrate_catalog_group_node_type_215_216.sql
```

### 上级平台 `register_expires`（REGISTER 有效期，秒）

- **脚本**: `backend/sql/migrate_upstream_register_expires.sql`
- **作用**: `upstream_platforms.register_expires`，默认 3600；未执行则列表接口查询会缺列报错。

```bash
psql "$DATABASE_URL" -v ON_ERROR_STOP=1 -f backend/sql/migrate_upstream_register_expires.sql
```

## 后端单元测试（编组国标纯逻辑）

```bash
cd backend/build && cmake .. -DGB_SERVICE_BUILD_TESTS=ON && cmake --build . && ctest --output-on-failure
```

可执行文件：`backend/build/catalog_group_encoding_test`（不依赖数据库）。
