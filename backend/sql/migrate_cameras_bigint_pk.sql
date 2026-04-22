-- cameras 主键迁移：VARCHAR(国标 DeviceID) -> BIGSERIAL，业务唯一 (platform_gb_id, device_gb_id)
-- 执行前请全库备份；建议在低峰/维护窗口执行。
-- 依赖：PostgreSQL，已有 cameras、replay_tasks；catalog_node_cameras / stream_sessions 可选（不存在则跳过对应段）。

BEGIN;

-- 1) 子表外键（replay_tasks 必选；catalog_node_cameras 若存在则解除）
ALTER TABLE replay_tasks DROP CONSTRAINT IF EXISTS replay_tasks_camera_id_fkey;

DO $$
BEGIN
  IF to_regclass('public.catalog_node_cameras') IS NOT NULL THEN
    EXECUTE 'ALTER TABLE catalog_node_cameras DROP CONSTRAINT IF EXISTS catalog_node_cameras_camera_id_fkey';
  END IF;
END $$;

-- 2) cameras：补齐 device_gb_id 与 platform_gb_id
ALTER TABLE cameras ADD COLUMN IF NOT EXISTS device_gb_id VARCHAR(32);
UPDATE cameras SET device_gb_id = TRIM(id::text) WHERE device_gb_id IS NULL OR TRIM(device_gb_id) = '';
UPDATE cameras c
SET platform_gb_id = TRIM(p.gb_id)
FROM device_platforms p
WHERE c.platform_id IS NOT NULL AND p.id = c.platform_id
  AND (c.platform_gb_id IS NULL OR TRIM(c.platform_gb_id) = '');

-- 无平台行时无法保证唯一键：需人工处理后再继续
DO $$
BEGIN
  IF EXISTS (SELECT 1 FROM cameras WHERE platform_id IS NULL OR TRIM(COALESCE(platform_gb_id,'')) = '') THEN
    RAISE EXCEPTION '迁移中止：存在 platform_id 或 platform_gb_id 为空的 cameras 行，请先补全';
  END IF;
END $$;

-- 3) 代理主键列（若已执行过一半可手工清理后重跑）
ALTER TABLE cameras ADD COLUMN IF NOT EXISTS camera_pk BIGSERIAL;

-- 4) 子表映射列（catalog_node_cameras 可选）
DO $$
BEGIN
  IF to_regclass('public.catalog_node_cameras') IS NOT NULL THEN
    EXECUTE 'ALTER TABLE catalog_node_cameras ADD COLUMN IF NOT EXISTS camera_db_id BIGINT';
    EXECUTE $u$
      UPDATE catalog_node_cameras cnc
      SET camera_db_id = c.camera_pk
      FROM cameras c
      WHERE cnc.camera_id::text = c.id::text OR cnc.camera_id::text = c.device_gb_id::text
    $u$;
    EXECUTE 'DELETE FROM catalog_node_cameras WHERE camera_db_id IS NULL';
    EXECUTE 'ALTER TABLE catalog_node_cameras DROP COLUMN IF EXISTS camera_id';
    EXECUTE 'ALTER TABLE catalog_node_cameras RENAME COLUMN camera_db_id TO camera_id';
    EXECUTE 'ALTER TABLE catalog_node_cameras ALTER COLUMN camera_id SET NOT NULL';
  END IF;
END $$;

ALTER TABLE replay_tasks ADD COLUMN IF NOT EXISTS camera_db_id BIGINT;
UPDATE replay_tasks rt
SET camera_db_id = c.camera_pk
FROM cameras c
WHERE rt.camera_id::text = c.id::text OR rt.camera_id::text = c.device_gb_id::text;

-- 5) 去掉无法映射的任务（孤儿数据）
DELETE FROM replay_tasks WHERE camera_db_id IS NULL;

-- 6) replay_tasks 切换列到 BIGINT
ALTER TABLE replay_tasks DROP COLUMN IF EXISTS camera_id;
ALTER TABLE replay_tasks RENAME COLUMN camera_db_id TO camera_id;
ALTER TABLE replay_tasks ALTER COLUMN camera_id SET NOT NULL;

-- 7) cameras 表：删除旧主键与旧 id 列，camera_pk -> id
ALTER TABLE cameras DROP CONSTRAINT IF EXISTS cameras_pkey;
ALTER TABLE cameras DROP COLUMN IF EXISTS id;
ALTER TABLE cameras RENAME COLUMN camera_pk TO id;
ALTER TABLE cameras ADD PRIMARY KEY (id);

ALTER TABLE cameras ALTER COLUMN device_gb_id SET NOT NULL;
ALTER TABLE cameras ALTER COLUMN platform_gb_id SET NOT NULL;
ALTER TABLE cameras ALTER COLUMN platform_id SET NOT NULL;

CREATE UNIQUE INDEX IF NOT EXISTS uq_cameras_platform_gb_device_gb ON cameras (platform_gb_id, device_gb_id);

-- 8) 子表外键
DO $$
BEGIN
  IF to_regclass('public.catalog_node_cameras') IS NOT NULL THEN
    EXECUTE 'ALTER TABLE catalog_node_cameras DROP CONSTRAINT IF EXISTS catalog_node_cameras_camera_id_fkey';
    EXECUTE 'ALTER TABLE catalog_node_cameras ADD CONSTRAINT catalog_node_cameras_camera_id_fkey FOREIGN KEY (camera_id) REFERENCES cameras(id) ON DELETE CASCADE';
  END IF;
END $$;

ALTER TABLE replay_tasks
  ADD CONSTRAINT replay_tasks_camera_id_fkey
  FOREIGN KEY (camera_id) REFERENCES cameras(id) ON DELETE CASCADE;

-- 9) stream_sessions：补充 camera_db_id（表可选）
DO $$
BEGIN
  IF to_regclass('public.stream_sessions') IS NOT NULL THEN
    EXECUTE 'ALTER TABLE stream_sessions ADD COLUMN IF NOT EXISTS camera_db_id BIGINT';
    EXECUTE $u$
      UPDATE stream_sessions ss
      SET camera_db_id = c.id
      FROM cameras c
      WHERE ss.device_gb_id::text = c.device_gb_id::text
        AND ss.platform_gb_id::text = c.platform_gb_id::text
    $u$;
    EXECUTE 'CREATE INDEX IF NOT EXISTS idx_stream_sessions_camera_db_id ON stream_sessions(camera_db_id)';
  END IF;
END $$;

COMMIT;
