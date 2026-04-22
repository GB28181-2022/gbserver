-- 上级平台 SIP REGISTER Expires（秒）
ALTER TABLE upstream_platforms
  ADD COLUMN IF NOT EXISTS register_expires INTEGER DEFAULT 3600;
