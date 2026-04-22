# cameras BIGINT 主键回滚说明

升级后 **`cameras.id` 已为 BIGSERIAL**，旧二进制（按 VARCHAR 国标主键编译）无法安全直连新库。

回滚方式（与计划一致）：

1. **优先**：在维护窗口从迁移前 **PostgreSQL 全量备份** 还原。
2. **不推荐**：手写逆向 DDL（需把 `replay_tasks` / `catalog_node_cameras` / `stream_sessions` 等外键改回 VARCHAR 并回填旧 `id`），易出错，仅建议在测试库演练并保留审计记录。

上线前请在**副本库**完整跑通 `migrate_cameras_bigint_pk.sql` 与业务回归，再执行生产迁移。
