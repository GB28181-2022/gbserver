-- 注册成功后是否主动下发 Catalog 查询，及按平台冷却（秒，0=关闭）
ALTER TABLE gb_local_config
  ADD COLUMN IF NOT EXISTS catalog_on_register_enabled BOOLEAN DEFAULT TRUE;
ALTER TABLE gb_local_config
  ADD COLUMN IF NOT EXISTS catalog_on_register_cooldown_sec INTEGER DEFAULT 60;
