-- 本机目录编组表（与下级同步 catalog_nodes 分离）
-- 执行: psql "$DATABASE_URL" -v ON_ERROR_STOP=1 -f backend/sql/migrate_add_catalog_group.sql
-- 幂等: 使用 IF NOT EXISTS

CREATE TABLE IF NOT EXISTS catalog_group_nodes (
  id BIGSERIAL PRIMARY KEY,
  parent_id BIGINT REFERENCES catalog_group_nodes(id) ON DELETE CASCADE,
  gb_device_id VARCHAR(32) NOT NULL,
  name VARCHAR(128) NOT NULL,
  node_type SMALLINT NOT NULL CHECK (node_type >= 0 AND node_type <= 3),
  civil_code VARCHAR(20),
  business_group_id VARCHAR(32),
  sort_order INT NOT NULL DEFAULT 0,
  source_platform_id BIGINT REFERENCES device_platforms(id) ON DELETE SET NULL,
  source_gb_device_id VARCHAR(32),
  created_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
  updated_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
  UNIQUE (gb_device_id)
);

CREATE INDEX IF NOT EXISTS idx_catalog_group_nodes_parent ON catalog_group_nodes(parent_id);
CREATE INDEX IF NOT EXISTS idx_catalog_group_nodes_parent_sort ON catalog_group_nodes(parent_id, sort_order);
CREATE INDEX IF NOT EXISTS idx_catalog_group_nodes_source ON catalog_group_nodes(source_platform_id, source_gb_device_id);

CREATE TABLE IF NOT EXISTS catalog_group_node_cameras (
  id BIGSERIAL PRIMARY KEY,
  group_node_id BIGINT NOT NULL REFERENCES catalog_group_nodes(id) ON DELETE CASCADE,
  camera_id BIGINT NOT NULL REFERENCES cameras(id) ON DELETE CASCADE,
  catalog_gb_device_id VARCHAR(32) NOT NULL,
  sort_order INT NOT NULL DEFAULT 0,
  source_platform_id BIGINT REFERENCES device_platforms(id) ON DELETE SET NULL,
  source_device_gb_id VARCHAR(32),
  created_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
  UNIQUE (group_node_id, camera_id),
  UNIQUE (catalog_gb_device_id)
);

CREATE INDEX IF NOT EXISTS idx_catalog_group_node_cameras_group ON catalog_group_node_cameras(group_node_id);
CREATE INDEX IF NOT EXISTS idx_catalog_group_node_cameras_camera ON catalog_group_node_cameras(camera_id);

COMMENT ON TABLE catalog_group_nodes IS '本机 GB 语义目录编组树（非下级 catalog_nodes 同步表）';
COMMENT ON TABLE catalog_group_node_cameras IS '编组节点挂载摄像头；catalog_gb_device_id 为编组用国标，与 cameras.device_gb_id 分离';
