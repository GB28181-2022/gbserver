-- GB28181 Catalog 节点表结构迁移脚本
-- 用于修复已部署环境中 catalog_nodes 表结构不完整的问题
-- 执行方式: psql -U user -d gb28181 -f migrate_catalog_nodes.sql

-- 开始事务
BEGIN;

-- ============================================
-- 步骤1: 检查并保存旧表数据（如果存在）
-- ============================================
DO $$
DECLARE
    old_table_exists BOOLEAN;
    temp_data_count INT;
BEGIN
    -- 检查旧表是否存在且需要迁移
    SELECT EXISTS (
        SELECT 1 FROM information_schema.columns 
        WHERE table_name = 'catalog_nodes' 
        AND column_name = 'code'
    ) INTO old_table_exists;
    
    IF old_table_exists THEN
        RAISE NOTICE '检测到旧版 catalog_nodes 表结构，需要迁移数据';
        
        -- 创建临时表保存旧数据
        DROP TABLE IF EXISTS _temp_catalog_nodes_old;
        CREATE TEMP TABLE _temp_catalog_nodes_old AS 
        SELECT * FROM catalog_nodes;
        
        SELECT COUNT(*) INTO temp_data_count FROM _temp_catalog_nodes_old;
        RAISE NOTICE '已备份 % 条旧数据到临时表', temp_data_count;
    END IF;
END $$;

-- ============================================
-- 步骤2: 删除旧表结构（如果存在冲突）
-- ============================================
-- 先删除外键约束（避免删除表时出错）
ALTER TABLE IF EXISTS catalog_node_cameras 
    DROP CONSTRAINT IF EXISTS catalog_node_cameras_node_id_fkey;

-- 删除旧表
DROP TABLE IF EXISTS catalog_nodes CASCADE;

-- ============================================
-- 步骤3: 创建新版 catalog_nodes 表结构
-- ============================================
CREATE TABLE catalog_nodes (
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

    -- 通用字段
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,

    -- 唯一约束：同一平台下节点ID不能重复
    UNIQUE(platform_id, node_id)
);

-- ============================================
-- 步骤4: 创建索引
-- ============================================
CREATE INDEX idx_catalog_nodes_platform ON catalog_nodes(platform_id);
CREATE INDEX idx_catalog_nodes_parent ON catalog_nodes(parent_id);
CREATE INDEX idx_catalog_nodes_type ON catalog_nodes(node_type);
CREATE INDEX idx_catalog_nodes_platform_type ON catalog_nodes(platform_id, node_type);

-- ============================================
-- 步骤5: 添加更新时间触发器
-- ============================================
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

-- ============================================
-- 步骤6: 恢复旧数据（如果有）
-- ============================================
DO $$
DECLARE
    temp_exists BOOLEAN;
    migrated_count INT := 0;
    old_record RECORD;
    target_platform_id BIGINT;
BEGIN
    SELECT EXISTS (
        SELECT 1 FROM information_schema.tables 
        WHERE table_name = '_temp_catalog_nodes_old'
    ) INTO temp_exists;
    
    IF temp_exists THEN
        RAISE NOTICE '开始恢复旧数据...';
        
        -- 尝试从关联关系推断平台ID
        FOR old_record IN SELECT * FROM _temp_catalog_nodes_old LOOP
            -- 尝试通过 camera 关联找到 platform_id
            SELECT cp.platform_id INTO target_platform_id
            FROM catalog_node_cameras cnc
            JOIN cameras cp ON cnc.camera_id = cp.id
            WHERE cnc.node_id = old_record.id
            LIMIT 1;
            
            -- 如果没找到，使用默认值
            IF target_platform_id IS NULL THEN
                target_platform_id := 0;
            END IF;
            
            -- 插入新表（旧数据简化存储）
            INSERT INTO catalog_nodes (
                node_id, platform_id, name, node_type, 
                created_at, updated_at
            ) VALUES (
                COALESCE(old_record.code, old_record.id::varchar), 
                target_platform_id,
                old_record.name, 
                1,  -- 默认作为目录类型
                old_record.created_at,
                old_record.updated_at
            )
            ON CONFLICT (platform_id, node_id) DO NOTHING;
            
            migrated_count := migrated_count + 1;
        END LOOP;
        
        RAISE NOTICE '已迁移 % 条旧数据', migrated_count;
        
        -- 删除临时表
        DROP TABLE IF EXISTS _temp_catalog_nodes_old;
    END IF;
END $$;

-- ============================================
-- 步骤7: 恢复 catalog_node_cameras 外键
-- ============================================
-- 如果表存在，恢复外键约束
DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM information_schema.tables WHERE table_name = 'catalog_node_cameras') THEN
        ALTER TABLE catalog_node_cameras 
        ADD CONSTRAINT catalog_node_cameras_node_id_fkey 
        FOREIGN KEY (node_id) REFERENCES catalog_nodes(id) ON DELETE CASCADE;
    END IF;
END $$;

-- ============================================
-- 步骤8: 添加表注释
-- ============================================
COMMENT ON TABLE catalog_nodes IS 'GB28181 目录树节点表，存储平台下发的完整目录结构';
COMMENT ON COLUMN catalog_nodes.node_id IS '节点国标ID（DeviceID）';
COMMENT ON COLUMN catalog_nodes.node_type IS '节点类型：0=设备(摄像头), 1=目录/组织, 2=行政区域';
COMMENT ON COLUMN catalog_nodes.platform_id IS '所属平台ID，关联 device_platforms';
COMMENT ON COLUMN catalog_nodes.civil_code IS '6位行政区划码';

-- 提交事务
COMMIT;

-- 输出完成信息
\echo '============================================'
\echo 'Catalog 节点表结构迁移完成！'
\echo '============================================'
\echo '新表结构支持：'
\echo '  - 20位国标设备编码（11x/12x/13x/21x/22x）'
\echo '  - 虚拟目录(215)和虚拟分组(216)'
\echo '  - 摄像机(131)、IPC(132)、NVR(121)等设备类型'
\echo '  - 行政区划节点(21x)'
\echo '  - 非20位编码的兼容处理（结合civil_code）'
\echo '============================================'
