-- GB28181 目录树节点表
-- 用于存储平台下发的完整目录结构，包括设备、目录、行政区域

CREATE TABLE IF NOT EXISTS catalog_nodes (
    id SERIAL PRIMARY KEY,
    node_id VARCHAR(32) NOT NULL,           -- 节点国标ID（DeviceID）
    platform_id BIGINT REFERENCES device_platforms(id) ON DELETE CASCADE,
    platform_gb_id VARCHAR(20),             -- 平台国标ID
    parent_id VARCHAR(32),                  -- 父节点ID（ParentID）
    name VARCHAR(128) NOT NULL,             -- 节点名称
    node_type INTEGER NOT NULL DEFAULT 0,   -- 节点类型：0=设备, 1=目录, 2=行政区域
    
    -- 设备特有字段（仅 node_type=0 时有效）
    manufacturer VARCHAR(64),               -- 设备厂商
    model VARCHAR(64),                      -- 设备型号
    owner VARCHAR(32),                      -- 设备归属
    civil_code VARCHAR(6),                  -- 行政区划码
    address VARCHAR(256),                   -- 安装地址
    parental INTEGER DEFAULT 0,             -- 是否有子设备（0=无, 1=有）
    safety_way INTEGER DEFAULT 0,           -- 信令安全模式
    register_way INTEGER DEFAULT 1,         -- 注册方式
    secrecy VARCHAR(1) DEFAULT '0',       -- 保密属性
    status VARCHAR(16),                     -- 设备状态（OK/ON/OFF/ERROR）
    longitude DOUBLE PRECISION,             -- 经度
    latitude DOUBLE PRECISION,              -- 纬度
    
    -- 目录/行政区域特有字段
    item_num INTEGER,                       -- 子节点数量（DeviceList Num）
    business_group_id VARCHAR(32),          -- 业务分组ID（BusinessGroupID）
    
    -- 通用字段
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    
    -- 唯一约束：同一平台下节点ID不能重复
    UNIQUE(platform_id, node_id)
);

-- 创建索引优化查询性能
CREATE INDEX IF NOT EXISTS idx_catalog_nodes_platform ON catalog_nodes(platform_id);
CREATE INDEX IF NOT EXISTS idx_catalog_nodes_parent ON catalog_nodes(parent_id);
CREATE INDEX IF NOT EXISTS idx_catalog_nodes_type ON catalog_nodes(node_type);
CREATE INDEX IF NOT EXISTS idx_catalog_nodes_platform_type ON catalog_nodes(platform_id, node_type);

-- 添加触发器自动更新 updated_at
CREATE OR REPLACE FUNCTION update_catalog_nodes_updated_at()
RETURNS TRIGGER AS $$
BEGIN
    NEW.updated_at = CURRENT_TIMESTAMP;
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trigger_catalog_nodes_updated_at ON catalog_nodes;
CREATE TRIGGER trigger_catalog_nodes_updated_at
    BEFORE UPDATE ON catalog_nodes
    FOR EACH ROW
    EXECUTE FUNCTION update_catalog_nodes_updated_at();

COMMENT ON TABLE catalog_nodes IS 'GB28181 目录树节点表，存储平台下发的完整目录结构';
COMMENT ON COLUMN catalog_nodes.node_type IS '节点类型：0=设备(摄像头), 1=目录/组织, 2=行政区域';
