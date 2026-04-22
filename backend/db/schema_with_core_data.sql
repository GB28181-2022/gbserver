--
-- PostgreSQL database dump
--

-- Dumped from database version 17.5 (Ubuntu 17.5-1.pgdg20.04+1)
-- Dumped by pg_dump version 17.5 (Ubuntu 17.5-1.pgdg20.04+1)

SET statement_timeout = 0;
SET lock_timeout = 0;
SET idle_in_transaction_session_timeout = 0;
SET transaction_timeout = 0;
SET client_encoding = 'UTF8';
SET standard_conforming_strings = on;
SELECT pg_catalog.set_config('search_path', '', false);
SET check_function_bodies = false;
SET xmloption = content;
SET client_min_messages = warning;
SET row_security = off;

--
-- Name: update_catalog_nodes_updated_at(); Type: FUNCTION; Schema: public; Owner: -
--

CREATE FUNCTION public.update_catalog_nodes_updated_at() RETURNS trigger
    LANGUAGE plpgsql
    AS $$
BEGIN
    NEW.updated_at = CURRENT_TIMESTAMP;
    RETURN NEW;
END;
$$;


SET default_tablespace = '';

SET default_table_access_method = heap;

--
-- Name: alarms; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.alarms (
    id bigint NOT NULL,
    channel_id character varying(64),
    channel_name character varying(128),
    level character varying(16) DEFAULT 'info'::character varying NOT NULL,
    status character varying(16) DEFAULT 'new'::character varying NOT NULL,
    description character varying(512),
    occurred_at timestamp with time zone DEFAULT CURRENT_TIMESTAMP,
    ack_at timestamp with time zone,
    dispose_note character varying(512),
    created_at timestamp with time zone DEFAULT CURRENT_TIMESTAMP,
    updated_at timestamp with time zone DEFAULT CURRENT_TIMESTAMP
);


--
-- Name: alarms_id_seq; Type: SEQUENCE; Schema: public; Owner: -
--

CREATE SEQUENCE public.alarms_id_seq
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;


--
-- Name: alarms_id_seq; Type: SEQUENCE OWNED BY; Schema: public; Owner: -
--

ALTER SEQUENCE public.alarms_id_seq OWNED BY public.alarms.id;


--
-- Name: cameras; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.cameras (
    name character varying(128) NOT NULL,
    platform_id bigint NOT NULL,
    platform_gb_id character varying(20) NOT NULL,
    online boolean DEFAULT false,
    created_at timestamp with time zone DEFAULT CURRENT_TIMESTAMP,
    updated_at timestamp with time zone DEFAULT CURRENT_TIMESTAMP,
    node_id character varying(32),
    node_ref integer,
    manufacturer character varying(64),
    model character varying(64),
    owner character varying(32),
    civil_code character varying(6),
    address character varying(256),
    parental integer DEFAULT 0,
    parent_id character varying(128),
    safety_way integer DEFAULT 0,
    register_way integer DEFAULT 1,
    secrecy character varying(1) DEFAULT '0'::character varying,
    device_gb_id character varying(32) NOT NULL,
    id bigint NOT NULL
);


--
-- Name: COLUMN cameras.manufacturer; Type: COMMENT; Schema: public; Owner: -
--

COMMENT ON COLUMN public.cameras.manufacturer IS '设备厂商';


--
-- Name: COLUMN cameras.model; Type: COMMENT; Schema: public; Owner: -
--

COMMENT ON COLUMN public.cameras.model IS '设备型号';


--
-- Name: COLUMN cameras.owner; Type: COMMENT; Schema: public; Owner: -
--

COMMENT ON COLUMN public.cameras.owner IS '设备归属';


--
-- Name: COLUMN cameras.civil_code; Type: COMMENT; Schema: public; Owner: -
--

COMMENT ON COLUMN public.cameras.civil_code IS '行政区划码';


--
-- Name: COLUMN cameras.address; Type: COMMENT; Schema: public; Owner: -
--

COMMENT ON COLUMN public.cameras.address IS '安装地址';


--
-- Name: COLUMN cameras.parental; Type: COMMENT; Schema: public; Owner: -
--

COMMENT ON COLUMN public.cameras.parental IS '是否有子设备（0=无, 1=有）';


--
-- Name: COLUMN cameras.parent_id; Type: COMMENT; Schema: public; Owner: -
--

COMMENT ON COLUMN public.cameras.parent_id IS '父设备ID';


--
-- Name: COLUMN cameras.safety_way; Type: COMMENT; Schema: public; Owner: -
--

COMMENT ON COLUMN public.cameras.safety_way IS '信令安全模式';


--
-- Name: COLUMN cameras.register_way; Type: COMMENT; Schema: public; Owner: -
--

COMMENT ON COLUMN public.cameras.register_way IS '注册方式（1=RFC3261, 2=GB28181）';


--
-- Name: COLUMN cameras.secrecy; Type: COMMENT; Schema: public; Owner: -
--

COMMENT ON COLUMN public.cameras.secrecy IS '保密属性（0=不涉密, 1=涉密）';


--
-- Name: cameras_camera_pk_seq; Type: SEQUENCE; Schema: public; Owner: -
--

CREATE SEQUENCE public.cameras_camera_pk_seq
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;


--
-- Name: cameras_camera_pk_seq; Type: SEQUENCE OWNED BY; Schema: public; Owner: -
--

ALTER SEQUENCE public.cameras_camera_pk_seq OWNED BY public.cameras.id;


--
-- Name: catalog_group_node_cameras; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.catalog_group_node_cameras (
    id bigint NOT NULL,
    group_node_id bigint NOT NULL,
    camera_id bigint NOT NULL,
    catalog_gb_device_id character varying(32) NOT NULL,
    sort_order integer DEFAULT 0 NOT NULL,
    source_platform_id bigint,
    source_device_gb_id character varying(32),
    created_at timestamp with time zone DEFAULT CURRENT_TIMESTAMP
);


--
-- Name: TABLE catalog_group_node_cameras; Type: COMMENT; Schema: public; Owner: -
--

COMMENT ON TABLE public.catalog_group_node_cameras IS '编组节点挂载摄像头；catalog_gb_device_id 为编组用国标，与 cameras.device_gb_id 分离';


--
-- Name: catalog_group_node_cameras_id_seq; Type: SEQUENCE; Schema: public; Owner: -
--

CREATE SEQUENCE public.catalog_group_node_cameras_id_seq
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;


--
-- Name: catalog_group_node_cameras_id_seq; Type: SEQUENCE OWNED BY; Schema: public; Owner: -
--

ALTER SEQUENCE public.catalog_group_node_cameras_id_seq OWNED BY public.catalog_group_node_cameras.id;


--
-- Name: catalog_group_nodes; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.catalog_group_nodes (
    id bigint NOT NULL,
    parent_id bigint,
    gb_device_id character varying(32) NOT NULL,
    name character varying(128) NOT NULL,
    node_type smallint NOT NULL,
    civil_code character varying(20),
    business_group_id character varying(32),
    sort_order integer DEFAULT 0 NOT NULL,
    source_platform_id bigint,
    source_gb_device_id character varying(32),
    created_at timestamp with time zone DEFAULT CURRENT_TIMESTAMP,
    updated_at timestamp with time zone DEFAULT CURRENT_TIMESTAMP,
    CONSTRAINT catalog_group_nodes_node_type_check CHECK (((node_type >= 0) AND (node_type <= 3)))
);


--
-- Name: TABLE catalog_group_nodes; Type: COMMENT; Schema: public; Owner: -
--

COMMENT ON TABLE public.catalog_group_nodes IS '本机 GB 语义目录编组树（非下级 catalog_nodes 同步表）';


--
-- Name: catalog_group_nodes_id_seq; Type: SEQUENCE; Schema: public; Owner: -
--

CREATE SEQUENCE public.catalog_group_nodes_id_seq
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;


--
-- Name: catalog_group_nodes_id_seq; Type: SEQUENCE OWNED BY; Schema: public; Owner: -
--

ALTER SEQUENCE public.catalog_group_nodes_id_seq OWNED BY public.catalog_group_nodes.id;


--
-- Name: catalog_nodes; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.catalog_nodes (
    id integer NOT NULL,
    node_id character varying(32) NOT NULL,
    platform_id bigint,
    platform_gb_id character varying(20),
    parent_id character varying(128),
    name character varying(128) NOT NULL,
    node_type integer DEFAULT 0 NOT NULL,
    manufacturer character varying(64),
    model character varying(64),
    owner character varying(32),
    civil_code character varying(6),
    address character varying(256),
    parental integer DEFAULT 0,
    safety_way integer DEFAULT 0,
    register_way integer DEFAULT 1,
    secrecy character varying(1) DEFAULT '0'::character varying,
    status character varying(16),
    longitude double precision,
    latitude double precision,
    item_num integer,
    created_at timestamp with time zone DEFAULT CURRENT_TIMESTAMP,
    updated_at timestamp with time zone DEFAULT CURRENT_TIMESTAMP,
    business_group_id character varying(32),
    block character varying(16),
    cert_num character varying(64),
    certifiable integer DEFAULT 0,
    err_code character varying(8),
    err_time character varying(20),
    ip_address character varying(64),
    port integer,
    device_id character varying(32),
    item_index integer
);


--
-- Name: TABLE catalog_nodes; Type: COMMENT; Schema: public; Owner: -
--

COMMENT ON TABLE public.catalog_nodes IS 'GB28181 目录树节点表';


--
-- Name: COLUMN catalog_nodes.node_type; Type: COMMENT; Schema: public; Owner: -
--

COMMENT ON COLUMN public.catalog_nodes.node_type IS '节点类型：0=设备, 1=目录, 2=行政区域';


--
-- Name: COLUMN catalog_nodes.business_group_id; Type: COMMENT; Schema: public; Owner: -
--

COMMENT ON COLUMN public.catalog_nodes.business_group_id IS '业务分组ID（BusinessGroupID），GB28181目录查询返回的分组标识';


--
-- Name: COLUMN catalog_nodes.block; Type: COMMENT; Schema: public; Owner: -
--

COMMENT ON COLUMN public.catalog_nodes.block IS '封锁状态（ON=已封锁, OFF=未封锁）';


--
-- Name: COLUMN catalog_nodes.cert_num; Type: COMMENT; Schema: public; Owner: -
--

COMMENT ON COLUMN public.catalog_nodes.cert_num IS '设备证书编号';


--
-- Name: COLUMN catalog_nodes.certifiable; Type: COMMENT; Schema: public; Owner: -
--

COMMENT ON COLUMN public.catalog_nodes.certifiable IS '证书有效性（0=无效, 1=有效）';


--
-- Name: COLUMN catalog_nodes.err_code; Type: COMMENT; Schema: public; Owner: -
--

COMMENT ON COLUMN public.catalog_nodes.err_code IS '设备错误码';


--
-- Name: COLUMN catalog_nodes.err_time; Type: COMMENT; Schema: public; Owner: -
--

COMMENT ON COLUMN public.catalog_nodes.err_time IS '错误发生时间';


--
-- Name: COLUMN catalog_nodes.ip_address; Type: COMMENT; Schema: public; Owner: -
--

COMMENT ON COLUMN public.catalog_nodes.ip_address IS '设备IP地址';


--
-- Name: COLUMN catalog_nodes.port; Type: COMMENT; Schema: public; Owner: -
--

COMMENT ON COLUMN public.catalog_nodes.port IS '设备端口';


--
-- Name: COLUMN catalog_nodes.device_id; Type: COMMENT; Schema: public; Owner: -
--

COMMENT ON COLUMN public.catalog_nodes.device_id IS '设备国标ID（与node_id一致，用于明确标识设备）';


--
-- Name: catalog_nodes_id_seq; Type: SEQUENCE; Schema: public; Owner: -
--

CREATE SEQUENCE public.catalog_nodes_id_seq
    AS integer
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;


--
-- Name: catalog_nodes_id_seq; Type: SEQUENCE OWNED BY; Schema: public; Owner: -
--

ALTER SEQUENCE public.catalog_nodes_id_seq OWNED BY public.catalog_nodes.id;


--
-- Name: device_platforms; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.device_platforms (
    id bigint NOT NULL,
    name character varying(128) NOT NULL,
    gb_id character varying(20) NOT NULL,
    list_type character varying(16) DEFAULT 'normal'::character varying NOT NULL,
    strategy_mode character varying(16) DEFAULT 'inherit'::character varying NOT NULL,
    custom_media_host character varying(128),
    custom_media_port integer,
    custom_auth_password character varying(128),
    stream_media_url character varying(256),
    online boolean DEFAULT false,
    camera_count integer DEFAULT 0,
    created_at timestamp with time zone DEFAULT CURRENT_TIMESTAMP,
    updated_at timestamp with time zone DEFAULT CURRENT_TIMESTAMP,
    contact_ip character varying(128),
    contact_port integer,
    signal_src_ip character varying(64),
    signal_src_port integer,
    last_heartbeat_at timestamp with time zone,
    stream_rtp_transport character varying(8),
    CONSTRAINT device_platforms_stream_rtp_transport_check CHECK (((stream_rtp_transport IS NULL) OR ((stream_rtp_transport)::text = ANY ((ARRAY['udp'::character varying, 'tcp'::character varying])::text[]))))
);


--
-- Name: COLUMN device_platforms.stream_rtp_transport; Type: COMMENT; Schema: public; Owner: -
--

COMMENT ON COLUMN public.device_platforms.stream_rtp_transport IS '独立配置时覆盖系统 rtp_transport；NULL 表示跟随系统';


--
-- Name: device_platforms_id_seq; Type: SEQUENCE; Schema: public; Owner: -
--

CREATE SEQUENCE public.device_platforms_id_seq
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;


--
-- Name: device_platforms_id_seq; Type: SEQUENCE OWNED BY; Schema: public; Owner: -
--

ALTER SEQUENCE public.device_platforms_id_seq OWNED BY public.device_platforms.id;


--
-- Name: gb_local_config; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.gb_local_config (
    id smallint NOT NULL,
    gb_id character varying(20),
    domain character varying(20),
    name character varying(128),
    username character varying(64),
    password character varying(128),
    signal_ip character varying(128),
    signal_port integer,
    transport_udp boolean DEFAULT true,
    transport_tcp boolean DEFAULT false,
    updated_at timestamp with time zone DEFAULT CURRENT_TIMESTAMP,
    catalog_on_register_enabled boolean DEFAULT true,
    catalog_on_register_cooldown_sec integer DEFAULT 60
);


--
-- Name: media_config; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.media_config (
    id smallint NOT NULL,
    rtp_port_start integer,
    rtp_port_end integer,
    media_http_host character varying(128),
    updated_at timestamp with time zone DEFAULT CURRENT_TIMESTAMP,
    zlm_secret character varying(64),
    rtp_transport character varying(8) DEFAULT 'udp'::character varying NOT NULL,
    preview_invite_timeout_sec integer DEFAULT 45,
    media_api_url character varying(512),
    zlm_open_rtp_server_wait_sec integer DEFAULT 10,
    CONSTRAINT media_config_rtp_transport_check CHECK (((rtp_transport)::text = ANY ((ARRAY['udp'::character varying, 'tcp'::character varying])::text[])))
);


--
-- Name: COLUMN media_config.rtp_transport; Type: COMMENT; Schema: public; Owner: -
--

COMMENT ON COLUMN public.media_config.rtp_transport IS '点播 INVITE SDP / ZLM openRtpServer：系统默认 RTP 传输 udp|tcp';


--
-- Name: replay_downloads; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.replay_downloads (
    id bigint NOT NULL,
    segment_id bigint NOT NULL,
    status character varying(16) NOT NULL,
    progress integer DEFAULT 0,
    created_at timestamp with time zone DEFAULT CURRENT_TIMESTAMP,
    updated_at timestamp with time zone DEFAULT CURRENT_TIMESTAMP,
    file_path character varying(1024),
    download_url character varying(2048)
);


--
-- Name: replay_downloads_id_seq; Type: SEQUENCE; Schema: public; Owner: -
--

CREATE SEQUENCE public.replay_downloads_id_seq
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;


--
-- Name: replay_downloads_id_seq; Type: SEQUENCE OWNED BY; Schema: public; Owner: -
--

ALTER SEQUENCE public.replay_downloads_id_seq OWNED BY public.replay_downloads.id;


--
-- Name: replay_segments; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.replay_segments (
    id bigint NOT NULL,
    task_id bigint NOT NULL,
    segment_id character varying(64) NOT NULL,
    start_time timestamp with time zone NOT NULL,
    end_time timestamp with time zone NOT NULL,
    duration_seconds integer,
    downloadable boolean DEFAULT true,
    created_at timestamp with time zone DEFAULT CURRENT_TIMESTAMP
);


--
-- Name: replay_segments_id_seq; Type: SEQUENCE; Schema: public; Owner: -
--

CREATE SEQUENCE public.replay_segments_id_seq
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;


--
-- Name: replay_segments_id_seq; Type: SEQUENCE OWNED BY; Schema: public; Owner: -
--

ALTER SEQUENCE public.replay_segments_id_seq OWNED BY public.replay_segments.id;


--
-- Name: replay_tasks; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.replay_tasks (
    id bigint NOT NULL,
    task_id character varying(64) NOT NULL,
    start_time timestamp with time zone NOT NULL,
    end_time timestamp with time zone NOT NULL,
    status character varying(16) NOT NULL,
    created_at timestamp with time zone DEFAULT CURRENT_TIMESTAMP,
    updated_at timestamp with time zone DEFAULT CURRENT_TIMESTAMP,
    camera_id bigint NOT NULL
);


--
-- Name: replay_tasks_id_seq; Type: SEQUENCE; Schema: public; Owner: -
--

CREATE SEQUENCE public.replay_tasks_id_seq
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;


--
-- Name: replay_tasks_id_seq; Type: SEQUENCE OWNED BY; Schema: public; Owner: -
--

ALTER SEQUENCE public.replay_tasks_id_seq OWNED BY public.replay_tasks.id;


--
-- Name: stream_sessions; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.stream_sessions (
    id integer NOT NULL,
    stream_id character varying(128) NOT NULL,
    camera_id character varying(64) NOT NULL,
    device_gb_id character varying(64) NOT NULL,
    platform_gb_id character varying(64) NOT NULL,
    zlm_port integer DEFAULT 0 NOT NULL,
    call_id character varying(128),
    status character varying(16) DEFAULT 'init'::character varying,
    flv_url character varying(256),
    viewer_count integer DEFAULT 0,
    is_active boolean DEFAULT false,
    updated_at timestamp without time zone DEFAULT CURRENT_TIMESTAMP,
    camera_db_id bigint
);


--
-- Name: stream_sessions_id_seq; Type: SEQUENCE; Schema: public; Owner: -
--

CREATE SEQUENCE public.stream_sessions_id_seq
    AS integer
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;


--
-- Name: stream_sessions_id_seq; Type: SEQUENCE OWNED BY; Schema: public; Owner: -
--

ALTER SEQUENCE public.stream_sessions_id_seq OWNED BY public.stream_sessions.id;


--
-- Name: upstream_catalog_camera_exclude; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.upstream_catalog_camera_exclude (
    upstream_platform_id bigint NOT NULL,
    camera_id bigint NOT NULL,
    created_at timestamp with time zone DEFAULT CURRENT_TIMESTAMP
);


--
-- Name: upstream_catalog_scope; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.upstream_catalog_scope (
    id bigint NOT NULL,
    upstream_platform_id bigint NOT NULL,
    catalog_group_node_id bigint NOT NULL,
    created_at timestamp with time zone DEFAULT CURRENT_TIMESTAMP
);


--
-- Name: upstream_catalog_scope_id_seq; Type: SEQUENCE; Schema: public; Owner: -
--

CREATE SEQUENCE public.upstream_catalog_scope_id_seq
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;


--
-- Name: upstream_catalog_scope_id_seq; Type: SEQUENCE OWNED BY; Schema: public; Owner: -
--

ALTER SEQUENCE public.upstream_catalog_scope_id_seq OWNED BY public.upstream_catalog_scope.id;


--
-- Name: upstream_platforms; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.upstream_platforms (
    id bigint NOT NULL,
    name character varying(128) NOT NULL,
    sip_domain character varying(20) NOT NULL,
    gb_id character varying(20) NOT NULL,
    sip_ip character varying(128) NOT NULL,
    sip_port integer NOT NULL,
    transport character varying(8) NOT NULL,
    reg_username character varying(64),
    reg_password character varying(128),
    enabled boolean DEFAULT true,
    heartbeat_interval integer DEFAULT 60,
    online boolean DEFAULT false,
    last_heartbeat_at timestamp with time zone,
    created_at timestamp with time zone DEFAULT CURRENT_TIMESTAMP,
    updated_at timestamp with time zone DEFAULT CURRENT_TIMESTAMP,
    register_expires integer DEFAULT 3600
);


--
-- Name: upstream_platforms_id_seq; Type: SEQUENCE; Schema: public; Owner: -
--

CREATE SEQUENCE public.upstream_platforms_id_seq
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;


--
-- Name: upstream_platforms_id_seq; Type: SEQUENCE OWNED BY; Schema: public; Owner: -
--

ALTER SEQUENCE public.upstream_platforms_id_seq OWNED BY public.upstream_platforms.id;


--
-- Name: user_tokens; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.user_tokens (
    id bigint NOT NULL,
    user_id bigint NOT NULL,
    token character varying(512) NOT NULL,
    expired_at timestamp with time zone,
    created_at timestamp with time zone DEFAULT CURRENT_TIMESTAMP
);


--
-- Name: user_tokens_id_seq; Type: SEQUENCE; Schema: public; Owner: -
--

CREATE SEQUENCE public.user_tokens_id_seq
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;


--
-- Name: user_tokens_id_seq; Type: SEQUENCE OWNED BY; Schema: public; Owner: -
--

ALTER SEQUENCE public.user_tokens_id_seq OWNED BY public.user_tokens.id;


--
-- Name: users; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.users (
    id bigint NOT NULL,
    username character varying(64) NOT NULL,
    password_hash character varying(255) NOT NULL,
    display_name character varying(128),
    roles character varying(255),
    created_at timestamp with time zone DEFAULT CURRENT_TIMESTAMP,
    updated_at timestamp with time zone DEFAULT CURRENT_TIMESTAMP
);


--
-- Name: users_id_seq; Type: SEQUENCE; Schema: public; Owner: -
--

CREATE SEQUENCE public.users_id_seq
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;


--
-- Name: users_id_seq; Type: SEQUENCE OWNED BY; Schema: public; Owner: -
--

ALTER SEQUENCE public.users_id_seq OWNED BY public.users.id;


--
-- Name: alarms id; Type: DEFAULT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.alarms ALTER COLUMN id SET DEFAULT nextval('public.alarms_id_seq'::regclass);


--
-- Name: cameras id; Type: DEFAULT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.cameras ALTER COLUMN id SET DEFAULT nextval('public.cameras_camera_pk_seq'::regclass);


--
-- Name: catalog_group_node_cameras id; Type: DEFAULT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.catalog_group_node_cameras ALTER COLUMN id SET DEFAULT nextval('public.catalog_group_node_cameras_id_seq'::regclass);


--
-- Name: catalog_group_nodes id; Type: DEFAULT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.catalog_group_nodes ALTER COLUMN id SET DEFAULT nextval('public.catalog_group_nodes_id_seq'::regclass);


--
-- Name: catalog_nodes id; Type: DEFAULT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.catalog_nodes ALTER COLUMN id SET DEFAULT nextval('public.catalog_nodes_id_seq'::regclass);


--
-- Name: device_platforms id; Type: DEFAULT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.device_platforms ALTER COLUMN id SET DEFAULT nextval('public.device_platforms_id_seq'::regclass);


--
-- Name: replay_downloads id; Type: DEFAULT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.replay_downloads ALTER COLUMN id SET DEFAULT nextval('public.replay_downloads_id_seq'::regclass);


--
-- Name: replay_segments id; Type: DEFAULT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.replay_segments ALTER COLUMN id SET DEFAULT nextval('public.replay_segments_id_seq'::regclass);


--
-- Name: replay_tasks id; Type: DEFAULT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.replay_tasks ALTER COLUMN id SET DEFAULT nextval('public.replay_tasks_id_seq'::regclass);


--
-- Name: stream_sessions id; Type: DEFAULT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.stream_sessions ALTER COLUMN id SET DEFAULT nextval('public.stream_sessions_id_seq'::regclass);


--
-- Name: upstream_catalog_scope id; Type: DEFAULT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.upstream_catalog_scope ALTER COLUMN id SET DEFAULT nextval('public.upstream_catalog_scope_id_seq'::regclass);


--
-- Name: upstream_platforms id; Type: DEFAULT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.upstream_platforms ALTER COLUMN id SET DEFAULT nextval('public.upstream_platforms_id_seq'::regclass);


--
-- Name: user_tokens id; Type: DEFAULT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.user_tokens ALTER COLUMN id SET DEFAULT nextval('public.user_tokens_id_seq'::regclass);


--
-- Name: users id; Type: DEFAULT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.users ALTER COLUMN id SET DEFAULT nextval('public.users_id_seq'::regclass);


--
-- Name: alarms alarms_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.alarms
    ADD CONSTRAINT alarms_pkey PRIMARY KEY (id);


--
-- Name: cameras cameras_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.cameras
    ADD CONSTRAINT cameras_pkey PRIMARY KEY (id);


--
-- Name: catalog_group_node_cameras catalog_group_node_cameras_catalog_gb_device_id_key; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.catalog_group_node_cameras
    ADD CONSTRAINT catalog_group_node_cameras_catalog_gb_device_id_key UNIQUE (catalog_gb_device_id);


--
-- Name: catalog_group_node_cameras catalog_group_node_cameras_group_node_id_camera_id_key; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.catalog_group_node_cameras
    ADD CONSTRAINT catalog_group_node_cameras_group_node_id_camera_id_key UNIQUE (group_node_id, camera_id);


--
-- Name: catalog_group_node_cameras catalog_group_node_cameras_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.catalog_group_node_cameras
    ADD CONSTRAINT catalog_group_node_cameras_pkey PRIMARY KEY (id);


--
-- Name: catalog_group_nodes catalog_group_nodes_gb_device_id_key; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.catalog_group_nodes
    ADD CONSTRAINT catalog_group_nodes_gb_device_id_key UNIQUE (gb_device_id);


--
-- Name: catalog_group_nodes catalog_group_nodes_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.catalog_group_nodes
    ADD CONSTRAINT catalog_group_nodes_pkey PRIMARY KEY (id);


--
-- Name: catalog_nodes catalog_nodes_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.catalog_nodes
    ADD CONSTRAINT catalog_nodes_pkey PRIMARY KEY (id);


--
-- Name: catalog_nodes catalog_nodes_platform_id_node_id_key; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.catalog_nodes
    ADD CONSTRAINT catalog_nodes_platform_id_node_id_key UNIQUE (platform_id, node_id);


--
-- Name: device_platforms device_platforms_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.device_platforms
    ADD CONSTRAINT device_platforms_pkey PRIMARY KEY (id);


--
-- Name: gb_local_config gb_local_config_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.gb_local_config
    ADD CONSTRAINT gb_local_config_pkey PRIMARY KEY (id);


--
-- Name: media_config media_config_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.media_config
    ADD CONSTRAINT media_config_pkey PRIMARY KEY (id);


--
-- Name: replay_downloads replay_downloads_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.replay_downloads
    ADD CONSTRAINT replay_downloads_pkey PRIMARY KEY (id);


--
-- Name: replay_segments replay_segments_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.replay_segments
    ADD CONSTRAINT replay_segments_pkey PRIMARY KEY (id);


--
-- Name: replay_tasks replay_tasks_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.replay_tasks
    ADD CONSTRAINT replay_tasks_pkey PRIMARY KEY (id);


--
-- Name: replay_tasks replay_tasks_task_id_key; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.replay_tasks
    ADD CONSTRAINT replay_tasks_task_id_key UNIQUE (task_id);


--
-- Name: stream_sessions stream_sessions_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.stream_sessions
    ADD CONSTRAINT stream_sessions_pkey PRIMARY KEY (id);


--
-- Name: stream_sessions stream_sessions_stream_id_key; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.stream_sessions
    ADD CONSTRAINT stream_sessions_stream_id_key UNIQUE (stream_id);


--
-- Name: upstream_catalog_camera_exclude upstream_catalog_camera_exclude_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.upstream_catalog_camera_exclude
    ADD CONSTRAINT upstream_catalog_camera_exclude_pkey PRIMARY KEY (upstream_platform_id, camera_id);


--
-- Name: upstream_catalog_scope upstream_catalog_scope_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.upstream_catalog_scope
    ADD CONSTRAINT upstream_catalog_scope_pkey PRIMARY KEY (id);


--
-- Name: upstream_catalog_scope upstream_catalog_scope_upstream_platform_id_catalog_group_n_key; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.upstream_catalog_scope
    ADD CONSTRAINT upstream_catalog_scope_upstream_platform_id_catalog_group_n_key UNIQUE (upstream_platform_id, catalog_group_node_id);


--
-- Name: upstream_platforms upstream_platforms_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.upstream_platforms
    ADD CONSTRAINT upstream_platforms_pkey PRIMARY KEY (id);


--
-- Name: user_tokens user_tokens_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.user_tokens
    ADD CONSTRAINT user_tokens_pkey PRIMARY KEY (id);


--
-- Name: users users_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.users
    ADD CONSTRAINT users_pkey PRIMARY KEY (id);


--
-- Name: users users_username_key; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.users
    ADD CONSTRAINT users_username_key UNIQUE (username);


--
-- Name: idx_alarms_occurred_at; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_alarms_occurred_at ON public.alarms USING btree (occurred_at DESC);


--
-- Name: idx_alarms_status; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_alarms_status ON public.alarms USING btree (status);


--
-- Name: idx_cameras_node_ref; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_cameras_node_ref ON public.cameras USING btree (node_ref);


--
-- Name: idx_cameras_platform_id; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_cameras_platform_id ON public.cameras USING btree (platform_id);


--
-- Name: idx_catalog_group_node_cameras_camera; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_catalog_group_node_cameras_camera ON public.catalog_group_node_cameras USING btree (camera_id);


--
-- Name: idx_catalog_group_node_cameras_camera_group; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_catalog_group_node_cameras_camera_group ON public.catalog_group_node_cameras USING btree (camera_id, group_node_id);


--
-- Name: idx_catalog_group_node_cameras_group; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_catalog_group_node_cameras_group ON public.catalog_group_node_cameras USING btree (group_node_id);


--
-- Name: idx_catalog_group_node_cameras_group_sort_id; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_catalog_group_node_cameras_group_sort_id ON public.catalog_group_node_cameras USING btree (group_node_id, sort_order, id);


--
-- Name: idx_catalog_group_nodes_parent; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_catalog_group_nodes_parent ON public.catalog_group_nodes USING btree (parent_id);


--
-- Name: idx_catalog_group_nodes_parent_sort; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_catalog_group_nodes_parent_sort ON public.catalog_group_nodes USING btree (parent_id, sort_order);


--
-- Name: idx_catalog_group_nodes_source; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_catalog_group_nodes_source ON public.catalog_group_nodes USING btree (source_platform_id, source_gb_device_id);


--
-- Name: idx_catalog_nodes_parent; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_catalog_nodes_parent ON public.catalog_nodes USING btree (parent_id);


--
-- Name: idx_catalog_nodes_platform; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_catalog_nodes_platform ON public.catalog_nodes USING btree (platform_id);


--
-- Name: idx_catalog_nodes_platform_type; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_catalog_nodes_platform_type ON public.catalog_nodes USING btree (platform_id, node_type);


--
-- Name: idx_catalog_nodes_type; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_catalog_nodes_type ON public.catalog_nodes USING btree (node_type);


--
-- Name: idx_device_platforms_gb_id; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_device_platforms_gb_id ON public.device_platforms USING btree (gb_id);


--
-- Name: idx_replay_segments_task_id; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_replay_segments_task_id ON public.replay_segments USING btree (task_id);


--
-- Name: idx_stream_sessions_call_id; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_stream_sessions_call_id ON public.stream_sessions USING btree (call_id);


--
-- Name: idx_stream_sessions_camera_db_id; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_stream_sessions_camera_db_id ON public.stream_sessions USING btree (camera_db_id);


--
-- Name: idx_stream_sessions_stream_id; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_stream_sessions_stream_id ON public.stream_sessions USING btree (stream_id);


--
-- Name: idx_upstream_cat_cam_excl_upstream; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_upstream_cat_cam_excl_upstream ON public.upstream_catalog_camera_exclude USING btree (upstream_platform_id);


--
-- Name: idx_upstream_catalog_scope_upstream; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_upstream_catalog_scope_upstream ON public.upstream_catalog_scope USING btree (upstream_platform_id);


--
-- Name: idx_user_tokens_user_id; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_user_tokens_user_id ON public.user_tokens USING btree (user_id);


--
-- Name: uq_cameras_platform_gb_device_gb; Type: INDEX; Schema: public; Owner: -
--

CREATE UNIQUE INDEX uq_cameras_platform_gb_device_gb ON public.cameras USING btree (platform_gb_id, device_gb_id);


--
-- Name: cameras cameras_node_ref_fkey; Type: FK CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.cameras
    ADD CONSTRAINT cameras_node_ref_fkey FOREIGN KEY (node_ref) REFERENCES public.catalog_nodes(id) ON DELETE SET NULL;


--
-- Name: cameras cameras_platform_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.cameras
    ADD CONSTRAINT cameras_platform_id_fkey FOREIGN KEY (platform_id) REFERENCES public.device_platforms(id) ON DELETE SET NULL;


--
-- Name: catalog_group_node_cameras catalog_group_node_cameras_camera_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.catalog_group_node_cameras
    ADD CONSTRAINT catalog_group_node_cameras_camera_id_fkey FOREIGN KEY (camera_id) REFERENCES public.cameras(id) ON DELETE CASCADE;


--
-- Name: catalog_group_node_cameras catalog_group_node_cameras_group_node_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.catalog_group_node_cameras
    ADD CONSTRAINT catalog_group_node_cameras_group_node_id_fkey FOREIGN KEY (group_node_id) REFERENCES public.catalog_group_nodes(id) ON DELETE CASCADE;


--
-- Name: catalog_group_node_cameras catalog_group_node_cameras_source_platform_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.catalog_group_node_cameras
    ADD CONSTRAINT catalog_group_node_cameras_source_platform_id_fkey FOREIGN KEY (source_platform_id) REFERENCES public.device_platforms(id) ON DELETE SET NULL;


--
-- Name: catalog_group_nodes catalog_group_nodes_parent_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.catalog_group_nodes
    ADD CONSTRAINT catalog_group_nodes_parent_id_fkey FOREIGN KEY (parent_id) REFERENCES public.catalog_group_nodes(id) ON DELETE CASCADE;


--
-- Name: catalog_group_nodes catalog_group_nodes_source_platform_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.catalog_group_nodes
    ADD CONSTRAINT catalog_group_nodes_source_platform_id_fkey FOREIGN KEY (source_platform_id) REFERENCES public.device_platforms(id) ON DELETE SET NULL;


--
-- Name: catalog_nodes catalog_nodes_platform_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.catalog_nodes
    ADD CONSTRAINT catalog_nodes_platform_id_fkey FOREIGN KEY (platform_id) REFERENCES public.device_platforms(id) ON DELETE CASCADE;


--
-- Name: replay_downloads replay_downloads_segment_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.replay_downloads
    ADD CONSTRAINT replay_downloads_segment_id_fkey FOREIGN KEY (segment_id) REFERENCES public.replay_segments(id) ON DELETE CASCADE;


--
-- Name: replay_segments replay_segments_task_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.replay_segments
    ADD CONSTRAINT replay_segments_task_id_fkey FOREIGN KEY (task_id) REFERENCES public.replay_tasks(id) ON DELETE CASCADE;


--
-- Name: replay_tasks replay_tasks_camera_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.replay_tasks
    ADD CONSTRAINT replay_tasks_camera_id_fkey FOREIGN KEY (camera_id) REFERENCES public.cameras(id) ON DELETE CASCADE;


--
-- Name: upstream_catalog_camera_exclude upstream_catalog_camera_exclude_camera_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.upstream_catalog_camera_exclude
    ADD CONSTRAINT upstream_catalog_camera_exclude_camera_id_fkey FOREIGN KEY (camera_id) REFERENCES public.cameras(id) ON DELETE CASCADE;


--
-- Name: upstream_catalog_camera_exclude upstream_catalog_camera_exclude_upstream_platform_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.upstream_catalog_camera_exclude
    ADD CONSTRAINT upstream_catalog_camera_exclude_upstream_platform_id_fkey FOREIGN KEY (upstream_platform_id) REFERENCES public.upstream_platforms(id) ON DELETE CASCADE;


--
-- Name: upstream_catalog_scope upstream_catalog_scope_catalog_group_node_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.upstream_catalog_scope
    ADD CONSTRAINT upstream_catalog_scope_catalog_group_node_id_fkey FOREIGN KEY (catalog_group_node_id) REFERENCES public.catalog_group_nodes(id) ON DELETE CASCADE;


--
-- Name: upstream_catalog_scope upstream_catalog_scope_upstream_platform_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.upstream_catalog_scope
    ADD CONSTRAINT upstream_catalog_scope_upstream_platform_id_fkey FOREIGN KEY (upstream_platform_id) REFERENCES public.upstream_platforms(id) ON DELETE CASCADE;


--
-- Name: user_tokens user_tokens_user_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.user_tokens
    ADD CONSTRAINT user_tokens_user_id_fkey FOREIGN KEY (user_id) REFERENCES public.users(id) ON DELETE CASCADE;


--
-- PostgreSQL database dump complete
--

--
-- PostgreSQL database dump
--

-- Dumped from database version 17.5 (Ubuntu 17.5-1.pgdg20.04+1)
-- Dumped by pg_dump version 17.5 (Ubuntu 17.5-1.pgdg20.04+1)

SET statement_timeout = 0;
SET lock_timeout = 0;
SET idle_in_transaction_session_timeout = 0;
SET transaction_timeout = 0;
SET client_encoding = 'UTF8';
SET standard_conforming_strings = on;
SELECT pg_catalog.set_config('search_path', '', false);
SET check_function_bodies = false;
SET xmloption = content;
SET client_min_messages = warning;
SET row_security = off;

--
-- Data for Name: gb_local_config; Type: TABLE DATA; Schema: public; Owner: -
--

INSERT INTO public.gb_local_config VALUES (1, '34020000002000000001', '3402000000', '34020000002000000001', '', 'admin', '192.168.1.9', 5060, true, false, '2026-04-22 10:05:49.446415+08', true, 60);


--
-- Data for Name: media_config; Type: TABLE DATA; Schema: public; Owner: -
--

INSERT INTO public.media_config VALUES (1, 30000, 30500, '192.168.1.9', '2026-04-22 10:05:49.455451+08', 'EEE7WmjcVqaNxGgnaJEsPqW4ENEmbLoW', 'udp', 45, 'http://127.0.0.1:880', 10);


--
-- Data for Name: users; Type: TABLE DATA; Schema: public; Owner: -
--

INSERT INTO public.users VALUES (1, 'admin', '7512184542f8c623950484992131596cea51e5619a2ef0e1d422debfc64d3623', '管理员', 'admin', '2026-03-13 14:35:06.641905+08', '2026-03-13 14:35:06.641905+08');


--
-- Name: users_id_seq; Type: SEQUENCE SET; Schema: public; Owner: -
--

SELECT pg_catalog.setval('public.users_id_seq', 1, true);


--
-- PostgreSQL database dump complete
--

