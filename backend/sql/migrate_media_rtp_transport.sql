-- RTP 传输：系统默认（media_config）与下级平台独立覆盖（device_platforms）
-- 执行：psql -f migrate_media_rtp_transport.sql <dbname>

ALTER TABLE media_config
  ADD COLUMN IF NOT EXISTS rtp_transport VARCHAR(8) NOT NULL DEFAULT 'udp'
  CHECK (rtp_transport IN ('udp', 'tcp'));

ALTER TABLE device_platforms
  ADD COLUMN IF NOT EXISTS stream_rtp_transport VARCHAR(8) NULL
  CHECK (stream_rtp_transport IS NULL OR stream_rtp_transport IN ('udp', 'tcp'));

COMMENT ON COLUMN media_config.rtp_transport IS '点播 INVITE SDP / ZLM openRtpServer：系统默认 RTP 传输 udp|tcp';
COMMENT ON COLUMN device_platforms.stream_rtp_transport IS '独立配置时覆盖系统 rtp_transport；NULL 表示跟随系统';
