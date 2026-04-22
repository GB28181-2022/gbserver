-- GB28181-2016 设备字段扩展迁移脚本（cameras表）
-- 为 cameras 表添加缺失的 GB28181 设备属性字段

-- 设备厂商
ALTER TABLE cameras ADD COLUMN IF NOT EXISTS manufacturer VARCHAR(64);
COMMENT ON COLUMN cameras.manufacturer IS '设备厂商';

-- 设备型号
ALTER TABLE cameras ADD COLUMN IF NOT EXISTS model VARCHAR(64);
COMMENT ON COLUMN cameras.model IS '设备型号';

-- 设备归属
ALTER TABLE cameras ADD COLUMN IF NOT EXISTS owner VARCHAR(32);
COMMENT ON COLUMN cameras.owner IS '设备归属';

-- 行政区划码
ALTER TABLE cameras ADD COLUMN IF NOT EXISTS civil_code VARCHAR(6);
COMMENT ON COLUMN cameras.civil_code IS '行政区划码';

-- 安装地址
ALTER TABLE cameras ADD COLUMN IF NOT EXISTS address VARCHAR(256);
COMMENT ON COLUMN cameras.address IS '安装地址';

-- 是否有子设备
ALTER TABLE cameras ADD COLUMN IF NOT EXISTS parental INTEGER DEFAULT 0;
COMMENT ON COLUMN cameras.parental IS '是否有子设备（0=无, 1=有）';

-- 父设备ID
ALTER TABLE cameras ADD COLUMN IF NOT EXISTS parent_id VARCHAR(32);
COMMENT ON COLUMN cameras.parent_id IS '父设备ID';

-- 信令安全模式
ALTER TABLE cameras ADD COLUMN IF NOT EXISTS safety_way INTEGER DEFAULT 0;
COMMENT ON COLUMN cameras.safety_way IS '信令安全模式';

-- 注册方式
ALTER TABLE cameras ADD COLUMN IF NOT EXISTS register_way INTEGER DEFAULT 1;
COMMENT ON COLUMN cameras.register_way IS '注册方式（1=RFC3261, 2=GB28181）';

-- 保密属性
ALTER TABLE cameras ADD COLUMN IF NOT EXISTS secrecy VARCHAR(1) DEFAULT '0';
COMMENT ON COLUMN cameras.secrecy IS '保密属性（0=不涉密, 1=涉密）';

-- 记录迁移完成
SELECT 'Migration completed: Added GB28181 device fields to cameras table' AS result;
