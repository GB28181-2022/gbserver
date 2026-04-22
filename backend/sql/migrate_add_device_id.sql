-- 为 catalog_nodes 表添加 device_id 字段
-- device_id 用于明确标识设备国标ID，与 node_id 保持一致

ALTER TABLE catalog_nodes ADD COLUMN IF NOT EXISTS device_id VARCHAR(32);
COMMENT ON COLUMN catalog_nodes.device_id IS '设备国标ID（与node_id一致，用于明确标识设备）';

-- 为已有数据填充 device_id（将 node_id 复制到 device_id）
UPDATE catalog_nodes SET device_id = node_id WHERE device_id IS NULL;

SELECT 'Migration completed: Added device_id column to catalog_nodes' AS result;
