-- 上级平台 gb_id 全局唯一（与 handlePostPlatform/handlePutPlatform 校验一致）
-- 若报错 duplicate key：先执行
--   SELECT gb_id, array_agg(id ORDER BY id) FROM upstream_platforms GROUP BY gb_id HAVING count(*)>1;
-- 手工删除或合并重复行后再执行本脚本。
ALTER TABLE upstream_platforms
  ADD CONSTRAINT upstream_platforms_gb_id_key UNIQUE (gb_id);
