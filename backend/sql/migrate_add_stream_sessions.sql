-- 视频预览流会话表迁移脚本
-- 用于存储GB28181视频点播的会话信息
-- 关联协议：GB/T 28181-2022 第8章 媒体传输

-- 创建流会话表
CREATE TABLE IF NOT EXISTS stream_sessions (
    id SERIAL PRIMARY KEY,
    stream_id VARCHAR(64) NOT NULL UNIQUE,     -- ZLM流标识（使用摄像头ID）
    camera_id VARCHAR(32) NOT NULL REFERENCES cameras(id) ON DELETE CASCADE,
    device_gb_id VARCHAR(32) NOT NULL,          -- 所属设备国标ID
    platform_gb_id VARCHAR(32) NOT NULL,        -- 所属平台国标ID
    zlm_port INTEGER NOT NULL,                  -- ZLM收流端口（RTP端口）
    call_id VARCHAR(128),                       -- SIP通话标识（用于BYE）
    status VARCHAR(16) DEFAULT 'init',          -- 会话状态：init/inviting/streaming/closing/closed
    flv_url VARCHAR(256),                       -- HTTP-FLV播放地址（Nginx代理格式）
    viewer_count INTEGER DEFAULT 0,             -- 观看者计数
    is_active BOOLEAN DEFAULT FALSE,            -- 是否正在推流
    error_message VARCHAR(256),                 -- 错误信息（如果失败）
    error_code INTEGER DEFAULT 0,               -- 错误码
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    expires_at TIMESTAMP                        -- 过期时间（用于自动清理）
);

-- 创建索引
CREATE INDEX IF NOT EXISTS idx_stream_sessions_camera ON stream_sessions(camera_id);
CREATE INDEX IF NOT EXISTS idx_stream_sessions_status ON stream_sessions(status);
CREATE INDEX IF NOT EXISTS idx_stream_sessions_stream_id ON stream_sessions(stream_id);
CREATE INDEX IF NOT EXISTS idx_stream_sessions_call_id ON stream_sessions(call_id);
CREATE INDEX IF NOT EXISTS idx_stream_sessions_created_at ON stream_sessions(created_at);

-- 添加表注释
COMMENT ON TABLE stream_sessions IS '视频预览流会话表 - 存储GB28181点播会话信息';
COMMENT ON COLUMN stream_sessions.stream_id IS 'ZLM流标识，使用摄像头ID作为唯一标识';
COMMENT ON COLUMN stream_sessions.zlm_port IS 'ZLM RTP收流端口，由openRtpServer分配';
COMMENT ON COLUMN stream_sessions.call_id IS 'SIP通话标识，用于发送BYE结束会话';
COMMENT ON COLUMN stream_sessions.status IS '会话状态：init初始/inviting邀请中/streaming播放中/closing关闭中/closed已关闭';
COMMENT ON COLUMN stream_sessions.flv_url IS 'HTTP-FLV播放地址，格式：http://{media_ip}/zlm/rtp/{stream_id}.live.flv';
COMMENT ON COLUMN stream_sessions.viewer_count IS '当前观看者数量，用于无人观看自动断流';

-- 创建自动更新updated_at的触发器
CREATE OR REPLACE FUNCTION update_stream_sessions_updated_at()
RETURNS TRIGGER AS $$
BEGIN
    NEW.updated_at = CURRENT_TIMESTAMP;
    RETURN NEW;
END;
$$ language 'plpgsql';

DROP TRIGGER IF EXISTS trg_stream_sessions_updated_at ON stream_sessions;

CREATE TRIGGER trg_stream_sessions_updated_at
    BEFORE UPDATE ON stream_sessions
    FOR EACH ROW
    EXECUTE FUNCTION update_stream_sessions_updated_at();

-- 输出完成信息
SELECT 'stream_sessions table and indexes created successfully' AS result;
