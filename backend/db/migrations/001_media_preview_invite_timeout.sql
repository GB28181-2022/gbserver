-- 预览会话：信令已建立但长期无 RTP 时的应用层超时（秒）
ALTER TABLE media_config ADD COLUMN IF NOT EXISTS preview_invite_timeout_sec INTEGER DEFAULT 45;
UPDATE media_config SET preview_invite_timeout_sec = 45 WHERE id = 1 AND preview_invite_timeout_sec IS NULL;
