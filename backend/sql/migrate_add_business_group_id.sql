-- 添加 business_group_id 字段到 catalog_nodes 表
-- 执行方式: psql -U user -d gb28181 -f migrate_add_business_group_id.sql

-- 检查并添加字段（如果不存在）
DO $$
BEGIN
    IF NOT EXISTS (
        SELECT 1 FROM information_schema.columns 
        WHERE table_name = 'catalog_nodes' 
        AND column_name = 'business_group_id'
    ) THEN
        ALTER TABLE catalog_nodes ADD COLUMN business_group_id VARCHAR(32);
        RAISE NOTICE '已添加 business_group_id 字段到 catalog_nodes 表';
    ELSE
        RAISE NOTICE 'business_group_id 字段已存在，无需修改';
    END IF;
END $$;

-- 添加字段注释
COMMENT ON COLUMN catalog_nodes.business_group_id IS '业务分组ID（BusinessGroupID），GB28181目录查询返回的分组标识';

\echo '迁移完成！'
