-- 迁移前自检（只读）：执行 migrate_cameras_bigint_pk.sql 前运行，根据结果清洗数据
SELECT 'cameras missing platform_id' AS check_name, COUNT(*) FROM cameras WHERE platform_id IS NULL;
SELECT 'cameras missing platform_gb_id' AS check_name, COUNT(*) FROM cameras WHERE platform_gb_id IS NULL OR TRIM(platform_gb_id) = '';
SELECT 'duplicate (platform_gb_id, device_gb_id) after fill' AS check_name, COUNT(*) FROM (
  SELECT platform_gb_id, device_gb_id, COUNT(*) c FROM cameras
  WHERE device_gb_id IS NOT NULL AND TRIM(platform_gb_id) <> ''
  GROUP BY 1, 2 HAVING COUNT(*) > 1
) t;
SELECT 'device_platforms duplicate gb_id' AS check_name, COUNT(*) FROM (
  SELECT gb_id, COUNT(*) c FROM device_platforms GROUP BY 1 HAVING COUNT(*) > 1
) t;
