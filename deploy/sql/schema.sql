-- 核心表骨架（PostgreSQL）

-- 1. 认证与账号
CREATE TABLE IF NOT EXISTS users (
  id             BIGSERIAL PRIMARY KEY,
  username       VARCHAR(64) NOT NULL UNIQUE,
  password_hash  VARCHAR(255) NOT NULL,
  display_name   VARCHAR(128),
  roles          VARCHAR(255),
  created_at     TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
  updated_at     TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS user_tokens (
  id          BIGSERIAL PRIMARY KEY,
  user_id     BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  token       VARCHAR(512) NOT NULL,
  expired_at  TIMESTAMPTZ,
  created_at  TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX IF NOT EXISTS idx_user_tokens_user_id ON user_tokens(user_id);

-- 2. 本地国标与流媒体配置
CREATE TABLE IF NOT EXISTS gb_local_config (
  id              SMALLINT PRIMARY KEY,
  gb_id           VARCHAR(20),
  domain          VARCHAR(20),
  name            VARCHAR(128),
  username        VARCHAR(64),
  password        VARCHAR(128),
  signal_ip       VARCHAR(128),
  signal_port     INTEGER,
  transport_udp   BOOLEAN DEFAULT TRUE,
  transport_tcp   BOOLEAN DEFAULT FALSE,
  catalog_on_register_enabled   BOOLEAN DEFAULT TRUE,
  catalog_on_register_cooldown_sec INTEGER DEFAULT 60,
  updated_at      TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS media_config (
  id               SMALLINT PRIMARY KEY,
  rtp_port_start   INTEGER,
  rtp_port_end     INTEGER,
  media_http_host  VARCHAR(128),
  media_api_url    VARCHAR(512),
  zlm_secret       VARCHAR(64),    -- ZLM API 密钥 (新版本 ZLM 必需)
  rtp_transport    VARCHAR(8) NOT NULL DEFAULT 'udp' CHECK (rtp_transport IN ('udp','tcp')),
  preview_invite_timeout_sec INTEGER DEFAULT 45,  -- 预览 INVITING 无 RTP 超时（秒），10～600
  zlm_open_rtp_server_wait_sec INTEGER DEFAULT 10, -- 下级 200 后等待 ZLM 源流就绪（秒），与 ZLM openRtp 收流超时同量级
  updated_at       TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP
);

-- 3. 上级平台接入
CREATE TABLE IF NOT EXISTS upstream_platforms (
  id                 BIGSERIAL PRIMARY KEY,
  name               VARCHAR(128) NOT NULL,
  sip_domain         VARCHAR(20) NOT NULL,
  gb_id              VARCHAR(20) NOT NULL UNIQUE,
  sip_ip             VARCHAR(128) NOT NULL,
  sip_port           INTEGER NOT NULL,
  transport          VARCHAR(8) NOT NULL,
  reg_username       VARCHAR(64),
  reg_password       VARCHAR(128),
  enabled            BOOLEAN DEFAULT TRUE,
  register_expires   INTEGER DEFAULT 3600,
  heartbeat_interval INTEGER DEFAULT 60,
  online             BOOLEAN DEFAULT FALSE,
  last_heartbeat_at  TIMESTAMPTZ,
  created_at         TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
  updated_at         TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS upstream_catalog_scope (
  id BIGSERIAL PRIMARY KEY,
  upstream_platform_id BIGINT NOT NULL REFERENCES upstream_platforms(id) ON DELETE CASCADE,
  catalog_group_node_id BIGINT NOT NULL REFERENCES catalog_group_nodes(id) ON DELETE CASCADE,
  created_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
  UNIQUE (upstream_platform_id, catalog_group_node_id)
);

CREATE INDEX IF NOT EXISTS idx_upstream_catalog_scope_upstream
  ON upstream_catalog_scope (upstream_platform_id);

-- 上级推送：在 scope 子树内排除指定摄像头（仍保留目录节点）
CREATE TABLE IF NOT EXISTS upstream_catalog_camera_exclude (
  upstream_platform_id BIGINT NOT NULL REFERENCES upstream_platforms(id) ON DELETE CASCADE,
  camera_id            BIGINT NOT NULL REFERENCES cameras(id) ON DELETE CASCADE,
  created_at           TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (upstream_platform_id, camera_id)
);

CREATE INDEX IF NOT EXISTS idx_upstream_cat_cam_excl_upstream
  ON upstream_catalog_camera_exclude (upstream_platform_id);

-- 4. 下级平台 / 设备平台
CREATE TABLE IF NOT EXISTS device_platforms (
  id                   BIGSERIAL PRIMARY KEY,
  name                 VARCHAR(128) NOT NULL,
  gb_id                VARCHAR(20) NOT NULL,
  list_type            VARCHAR(16) NOT NULL DEFAULT 'normal',
  strategy_mode        VARCHAR(16) NOT NULL DEFAULT 'inherit',
  custom_media_host    VARCHAR(128),
  custom_media_port    INTEGER,
  custom_auth_password VARCHAR(128),
  stream_media_url     VARCHAR(256),
  stream_rtp_transport VARCHAR(8) CHECK (stream_rtp_transport IS NULL OR stream_rtp_transport IN ('udp','tcp')),
  online               BOOLEAN DEFAULT FALSE,
  camera_count         INTEGER DEFAULT 0,
  contact_ip           VARCHAR(64),
  contact_port         INTEGER,
  signal_src_ip        VARCHAR(64),
  signal_src_port      INTEGER,
  last_heartbeat_at    TIMESTAMPTZ,
  created_at           TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
  updated_at           TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX IF NOT EXISTS idx_device_platforms_gb_id ON device_platforms(gb_id);

-- 5. 摄像头
-- 库内主键 id（BIGSERIAL）；业务唯一 (platform_gb_id, device_gb_id)；HTTP 路径 id 为库内主键
CREATE TABLE IF NOT EXISTS cameras (
  id               BIGSERIAL PRIMARY KEY,
  device_gb_id     VARCHAR(32) NOT NULL,
  name             VARCHAR(128) NOT NULL,
  platform_id      BIGINT NOT NULL REFERENCES device_platforms(id) ON DELETE CASCADE,
  platform_gb_id   VARCHAR(20) NOT NULL,
  online           BOOLEAN DEFAULT FALSE,

  node_id          VARCHAR(32),
  node_ref         INTEGER REFERENCES catalog_nodes(id) ON DELETE SET NULL,

  manufacturer     VARCHAR(64),
  model            VARCHAR(64),
  owner            VARCHAR(32),
  civil_code       VARCHAR(6),
  address          VARCHAR(256),
  parental         INTEGER DEFAULT 0,
  parent_id        VARCHAR(128),
  safety_way       INTEGER DEFAULT 0,
  register_way     INTEGER DEFAULT 1,
  secrecy          VARCHAR(1) DEFAULT '0',

  created_at       TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
  updated_at       TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
  UNIQUE (platform_gb_id, device_gb_id)
);

CREATE INDEX IF NOT EXISTS idx_cameras_platform_id ON cameras(platform_id);
CREATE INDEX IF NOT EXISTS idx_cameras_device_gb_id ON cameras(device_gb_id);

-- 6. 目录编组与挂载（GB28181 Catalog节点表）
-- 用于存储平台下发的完整目录结构，包括设备、目录、行政区域
CREATE TABLE IF NOT EXISTS catalog_nodes (
    id SERIAL PRIMARY KEY,
    node_id VARCHAR(32) NOT NULL,           -- 节点国标ID（DeviceID）
    device_id VARCHAR(32),                    -- 设备国标ID（与node_id一致，用于明确标识设备）
    platform_id BIGINT REFERENCES device_platforms(id) ON DELETE CASCADE,
    platform_gb_id VARCHAR(20),             -- 平台国标ID
    parent_id VARCHAR(128),                 -- 父节点ID（ParentID，支持多级路径如 id1/id2/id3）
    name VARCHAR(128) NOT NULL,             -- 节点名称
    node_type INTEGER NOT NULL DEFAULT 0,   -- 节点类型：0=设备, 1=目录, 2=行政区域

    -- 设备特有字段（仅 node_type=0 时有效）
    manufacturer VARCHAR(64),               -- 设备厂商
    model VARCHAR(64),                      -- 设备型号
    owner VARCHAR(32),                      -- 设备归属
    civil_code VARCHAR(6),                  -- 行政区划码
    address VARCHAR(256),                   -- 安装地址
    parental INTEGER DEFAULT 0,             -- 是否有子设备（0=无, 1=有）
    safety_way INTEGER DEFAULT 0,           -- 信令安全模式
    register_way INTEGER DEFAULT 1,         -- 注册方式
    secrecy VARCHAR(1) DEFAULT '0',       -- 保密属性
    status VARCHAR(16),                     -- 设备状态（OK/ON/OFF/ERROR）
    longitude DOUBLE PRECISION,             -- 经度
    latitude DOUBLE PRECISION,              -- 纬度
    
    -- GB28181-2016 扩展字段（设备安全相关）
    block VARCHAR(16),                      -- 封锁状态（ON/OFF）
    cert_num VARCHAR(64),                   -- 证书编号
    certifiable INTEGER DEFAULT 0,          -- 证书有效性（0=无效, 1=有效）
    err_code VARCHAR(8),                    -- 错误码
    err_time VARCHAR(20),                   -- 错误时间
    ip_address VARCHAR(64),                 -- 设备IP地址
    port INTEGER,                           -- 设备端口

    -- 目录/行政区域特有字段
    item_num INTEGER,                       -- 子节点数量（DeviceList Num）
    item_index INTEGER,                     -- 当前节点在设备列表中的索引（Item Index）
    business_group_id VARCHAR(32),          -- 业务分组ID（BusinessGroupID）

    -- 通用字段
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,

    -- 唯一约束：同一平台下节点ID不能重复
    UNIQUE(platform_id, node_id)
);

-- 创建索引优化查询性能
CREATE INDEX IF NOT EXISTS idx_catalog_nodes_platform ON catalog_nodes(platform_id);
CREATE INDEX IF NOT EXISTS idx_catalog_nodes_parent ON catalog_nodes(parent_id);
CREATE INDEX IF NOT EXISTS idx_catalog_nodes_type ON catalog_nodes(node_type);
CREATE INDEX IF NOT EXISTS idx_catalog_nodes_platform_type ON catalog_nodes(platform_id, node_type);

-- 添加触发器自动更新 updated_at
CREATE OR REPLACE FUNCTION update_catalog_nodes_updated_at()
RETURNS TRIGGER AS $$
BEGIN
    NEW.updated_at = CURRENT_TIMESTAMP;
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trigger_catalog_nodes_updated_at ON catalog_nodes;
CREATE TRIGGER trigger_catalog_nodes_updated_at
    BEFORE UPDATE ON catalog_nodes
    FOR EACH ROW
    EXECUTE FUNCTION update_catalog_nodes_updated_at();

-- 目录与摄像头关联表（用于前端编组功能）
CREATE TABLE IF NOT EXISTS catalog_node_cameras (
  id         BIGSERIAL PRIMARY KEY,
  node_id    BIGINT NOT NULL REFERENCES catalog_nodes(id) ON DELETE CASCADE,
  camera_id  BIGINT NOT NULL REFERENCES cameras(id) ON DELETE CASCADE,
  created_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
  UNIQUE (node_id, camera_id)
);

CREATE INDEX IF NOT EXISTS idx_catalog_node_cameras_node_id ON catalog_node_cameras(node_id);

-- 6b. 本机目录编组（GB28181 语义，与 catalog_nodes 下级同步分离）
CREATE TABLE IF NOT EXISTS catalog_group_nodes (
  id BIGSERIAL PRIMARY KEY,
  parent_id BIGINT REFERENCES catalog_group_nodes(id) ON DELETE CASCADE,
  gb_device_id VARCHAR(32) NOT NULL,
  name VARCHAR(128) NOT NULL,
  node_type SMALLINT NOT NULL CHECK (node_type >= 0 AND node_type <= 3),
  civil_code VARCHAR(20),
  business_group_id VARCHAR(32),
  sort_order INT NOT NULL DEFAULT 0,
  source_platform_id BIGINT REFERENCES device_platforms(id) ON DELETE SET NULL,
  source_gb_device_id VARCHAR(32),
  created_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
  updated_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
  UNIQUE (gb_device_id)
);

CREATE INDEX IF NOT EXISTS idx_catalog_group_nodes_parent ON catalog_group_nodes(parent_id);
CREATE INDEX IF NOT EXISTS idx_catalog_group_nodes_parent_sort ON catalog_group_nodes(parent_id, sort_order);
CREATE INDEX IF NOT EXISTS idx_catalog_group_nodes_source ON catalog_group_nodes(source_platform_id, source_gb_device_id);

CREATE TABLE IF NOT EXISTS catalog_group_node_cameras (
  id BIGSERIAL PRIMARY KEY,
  group_node_id BIGINT NOT NULL REFERENCES catalog_group_nodes(id) ON DELETE CASCADE,
  camera_id BIGINT NOT NULL REFERENCES cameras(id) ON DELETE CASCADE,
  catalog_gb_device_id VARCHAR(32) NOT NULL,
  sort_order INT NOT NULL DEFAULT 0,
  source_platform_id BIGINT REFERENCES device_platforms(id) ON DELETE SET NULL,
  source_device_gb_id VARCHAR(32),
  created_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
  UNIQUE (group_node_id, camera_id),
  UNIQUE (catalog_gb_device_id)
);

CREATE INDEX IF NOT EXISTS idx_catalog_group_node_cameras_group ON catalog_group_node_cameras(group_node_id);
CREATE INDEX IF NOT EXISTS idx_catalog_group_node_cameras_camera ON catalog_group_node_cameras(camera_id);
CREATE INDEX IF NOT EXISTS idx_catalog_group_node_cameras_group_sort_id
  ON catalog_group_node_cameras(group_node_id, sort_order, id);
CREATE INDEX IF NOT EXISTS idx_catalog_group_node_cameras_camera_group
  ON catalog_group_node_cameras(camera_id, group_node_id);

-- 7. 回放相关骨架
CREATE TABLE IF NOT EXISTS replay_tasks (
  id          BIGSERIAL PRIMARY KEY,
  task_id     VARCHAR(64) NOT NULL UNIQUE,
  camera_id   BIGINT NOT NULL REFERENCES cameras(id) ON DELETE CASCADE,
  start_time  TIMESTAMPTZ NOT NULL,
  end_time    TIMESTAMPTZ NOT NULL,
  status      VARCHAR(16) NOT NULL,
  created_at  TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
  updated_at  TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS replay_segments (
  id               BIGSERIAL PRIMARY KEY,
  task_id          BIGINT NOT NULL REFERENCES replay_tasks(id) ON DELETE CASCADE,
  segment_id       VARCHAR(64) NOT NULL,
  start_time       TIMESTAMPTZ NOT NULL,
  end_time         TIMESTAMPTZ NOT NULL,
  duration_seconds INTEGER,
  downloadable     BOOLEAN DEFAULT TRUE,
  created_at       TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX IF NOT EXISTS idx_replay_segments_task_id ON replay_segments(task_id);

CREATE TABLE IF NOT EXISTS replay_downloads (
  id          BIGSERIAL PRIMARY KEY,
  segment_id  BIGINT NOT NULL REFERENCES replay_segments(id) ON DELETE CASCADE,
  status      VARCHAR(16) NOT NULL,
  progress    INTEGER DEFAULT 0,
  created_at  TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
  updated_at  TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP
);

-- 8. 告警事件（SVR-501/502：接收、确认、处置、记录）
CREATE TABLE IF NOT EXISTS alarms (
  id           BIGSERIAL PRIMARY KEY,
  channel_id   VARCHAR(64),
  channel_name VARCHAR(128),
  level        VARCHAR(16) NOT NULL DEFAULT 'info',
  status       VARCHAR(16) NOT NULL DEFAULT 'new',
  description  VARCHAR(512),
  occurred_at  TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
  ack_at       TIMESTAMPTZ,
  dispose_note VARCHAR(512),
  created_at   TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
  updated_at   TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX IF NOT EXISTS idx_alarms_status ON alarms(status);
CREATE INDEX IF NOT EXISTS idx_alarms_occurred_at ON alarms(occurred_at DESC);

