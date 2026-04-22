-- 修改 cameras 表，添加与 catalog_nodes 的关联

-- 添加 node_id 字段，用于关联到 catalog_nodes
ALTER TABLE cameras 
ADD COLUMN IF NOT EXISTS node_id VARCHAR(32),
ADD COLUMN IF NOT EXISTS node_ref INTEGER REFERENCES catalog_nodes(id) ON DELETE SET NULL,
ADD COLUMN IF NOT EXISTS is_device BOOLEAN DEFAULT true;  -- true=真正的摄像头设备

-- 创建索引优化查询
CREATE INDEX IF NOT EXISTS idx_cameras_node_ref ON cameras(node_ref);
CREATE INDEX IF NOT EXISTS idx_cameras_is_device ON cameras(is_device) WHERE is_device = true;

-- 更新现有数据：将 node_id 设置为与 id 相同（兼容旧数据）
UPDATE cameras SET node_id = id WHERE node_id IS NULL;

COMMENT ON COLUMN cameras.node_id IS '关联到 catalog_nodes 的 node_id';
COMMENT ON COLUMN cameras.node_ref IS '关联到 catalog_nodes 的外键';
COMMENT ON COLUMN cameras.is_device IS '是否为真正的摄像头设备（用于过滤目录节点）';
