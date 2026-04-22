-- NAT 信令出口与心跳持久化（与 SipHandler / SipCatalog 一致）
-- PostgreSQL：可重复执行

ALTER TABLE device_platforms
    ADD COLUMN IF NOT EXISTS contact_ip VARCHAR(64),
    ADD COLUMN IF NOT EXISTS contact_port INTEGER,
    ADD COLUMN IF NOT EXISTS signal_src_ip VARCHAR(64),
    ADD COLUMN IF NOT EXISTS signal_src_port INTEGER,
    ADD COLUMN IF NOT EXISTS last_heartbeat_at TIMESTAMPTZ;

-- 历史列 src_ip/src_port（若曾执行 migration_add_src_addr.sql）
DO $$
BEGIN
  IF EXISTS (
    SELECT 1 FROM information_schema.columns
    WHERE table_schema = 'public' AND table_name = 'device_platforms' AND column_name = 'src_ip'
  ) THEN
    UPDATE device_platforms
    SET signal_src_ip = src_ip
    WHERE signal_src_ip IS NULL AND src_ip IS NOT NULL AND TRIM(src_ip) <> '';
  END IF;
  IF EXISTS (
    SELECT 1 FROM information_schema.columns
    WHERE table_schema = 'public' AND table_name = 'device_platforms' AND column_name = 'src_port'
  ) THEN
    UPDATE device_platforms
    SET signal_src_port = src_port
    WHERE signal_src_port IS NULL AND src_port IS NOT NULL AND src_port > 0;
  END IF;
END $$;
