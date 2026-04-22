-- GB28181-2016 设备字段扩展迁移脚本
-- 为 catalog_nodes 表添加缺失的设备安全相关字段

-- 添加封锁状态字段
ALTER TABLE catalog_nodes ADD COLUMN IF NOT EXISTS block VARCHAR(16);
COMMENT ON COLUMN catalog_nodes.block IS '封锁状态（ON=已封锁, OFF=未封锁）';

-- 添加证书编号字段
ALTER TABLE catalog_nodes ADD COLUMN IF NOT EXISTS cert_num VARCHAR(64);
COMMENT ON COLUMN catalog_nodes.cert_num IS '设备证书编号';

-- 添加证书有效性字段
ALTER TABLE catalog_nodes ADD COLUMN IF NOT EXISTS certifiable INTEGER DEFAULT 0;
COMMENT ON COLUMN catalog_nodes.certifiable IS '证书有效性（0=无效, 1=有效）';

-- 添加错误码字段
ALTER TABLE catalog_nodes ADD COLUMN IF NOT EXISTS err_code VARCHAR(8);
COMMENT ON COLUMN catalog_nodes.err_code IS '设备错误码';

-- 添加错误时间字段
ALTER TABLE catalog_nodes ADD COLUMN IF NOT EXISTS err_time VARCHAR(20);
COMMENT ON COLUMN catalog_nodes.err_time IS '错误发生时间';

-- 添加设备IP地址字段
ALTER TABLE catalog_nodes ADD COLUMN IF NOT EXISTS ip_address VARCHAR(64);
COMMENT ON COLUMN catalog_nodes.ip_address IS '设备IP地址';

-- 添加设备端口字段
ALTER TABLE catalog_nodes ADD COLUMN IF NOT EXISTS port INTEGER;
COMMENT ON COLUMN catalog_nodes.port IS '设备端口';

-- 记录迁移完成
SELECT 'Migration completed: Added device security fields to catalog_nodes' AS result;
