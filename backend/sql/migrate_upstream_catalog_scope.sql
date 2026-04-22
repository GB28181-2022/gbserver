-- 上级平台目录编组范围：多选 catalog_group_nodes（含子树由应用层展开）
CREATE TABLE IF NOT EXISTS upstream_catalog_scope (
  id BIGSERIAL PRIMARY KEY,
  upstream_platform_id BIGINT NOT NULL REFERENCES upstream_platforms(id) ON DELETE CASCADE,
  catalog_group_node_id BIGINT NOT NULL REFERENCES catalog_group_nodes(id) ON DELETE CASCADE,
  created_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
  UNIQUE (upstream_platform_id, catalog_group_node_id)
);

CREATE INDEX IF NOT EXISTS idx_upstream_catalog_scope_upstream
  ON upstream_catalog_scope (upstream_platform_id);
