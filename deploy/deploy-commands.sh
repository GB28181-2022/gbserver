#!/bin/bash
# GB28181 国标服务部署命令集
# 在 192.168.1.2 上执行此脚本

set -e

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }
log_step() { echo -e "${BLUE}[STEP]${NC} $1"; }

# 配置
INSTALL_DIR="/opt/gb_service2022"
ZLM_SECRET="sF3auJDv6AqFUs8BfrlLYmDt2H7S9MGC"
DB_NAME="gb28181"
DB_USER="user"
DB_PASS="gb28181_pass"

# 检查 root
if [ "$EUID" -ne 0 ]; then
    log_error "请使用 sudo 运行"
    exit 1
fi

# ========== 步骤 1: 解压部署包 ==========
log_step "步骤 1: 解压部署包..."
cd /tmp
if [ -f "gb28181-deploy-192.168.1.2.tar.gz" ]; then
    tar -xzvf gb28181-deploy-192.168.1.2.tar.gz
    cd deploy
    log_info "部署包解压完成"
else
    log_error "找不到部署包: /tmp/gb28181-deploy-192.168.1.2.tar.gz"
    exit 1
fi

# ========== 步骤 2: 安装依赖 ==========
log_step "步骤 2: 安装系统依赖..."
apt-get update
apt-get install -y libpq5 libcurl4 libssl1.1 libuuid1 postgresql postgresql-contrib nginx curl net-tools sshpass
systemctl enable postgresql
systemctl start postgresql
log_info "依赖安装完成"

# ========== 步骤 3: 初始化数据库 ==========
log_step "步骤 3: 初始化数据库..."
sudo -u postgres psql -c "CREATE USER $DB_USER WITH PASSWORD '$DB_PASS';" 2>/dev/null || log_warn "用户已存在"
sudo -u postgres psql -c "CREATE DATABASE $DB_NAME OWNER $DB_USER;" 2>/dev/null || log_warn "数据库已存在"
sudo -u postgres psql -c "GRANT ALL PRIVILEGES ON DATABASE $DB_NAME TO $DB_USER;"

# 导入表结构
sudo -u postgres psql -d $DB_NAME -f ./sql/schema.sql
log_info "数据库初始化完成"

# 插入初始配置
sudo -u postgres psql -d $DB_NAME << EOF
-- 国标本地配置
INSERT INTO gb_local_config (id, gb_id, domain, name, username, password, signal_ip, signal_port, transport_udp, transport_tcp)
VALUES (1, '34020000002000000001', '3402000000', '国标平台', 'admin', 'admin', '192.168.1.2', 5060, true, false)
ON CONFLICT (id) DO UPDATE SET
    gb_id = EXCLUDED.gb_id,
    domain = EXCLUDED.domain,
    signal_ip = EXCLUDED.signal_ip,
    signal_port = EXCLUDED.signal_port;

-- 媒体配置
INSERT INTO media_config (id, rtp_port_start, rtp_port_end, media_http_host, media_api_url, zlm_secret, rtp_transport)
VALUES (1, 30000, 35000, '192.168.1.2', 'http://127.0.0.1:880', '$ZLM_SECRET', 'udp')
ON CONFLICT (id) DO UPDATE SET
    media_http_host = EXCLUDED.media_http_host,
    media_api_url = EXCLUDED.media_api_url,
    zlm_secret = EXCLUDED.zlm_secret;
EOF

# ========== 步骤 4: 安装应用 ==========
log_step "步骤 4: 安装应用程序..."
mkdir -p $INSTALL_DIR/{backend,frontend,logs}
cp ./backend/gb_service $INSTALL_DIR/backend/
chmod +x $INSTALL_DIR/backend/gb_service
cp -r ./frontend/* $INSTALL_DIR/frontend/
touch /var/log/gb_service.log
chmod 666 /var/log/gb_service.log
log_info "应用程序安装完成"

# ========== 步骤 5: 配置 Nginx ==========
log_step "步骤 5: 配置 Nginx..."
cp ./config/gb_service.conf /etc/nginx/sites-available/

# 更新配置中的路径
sed -i "s|/home/\*\*\*/coder/gb_service2022|$INSTALL_DIR|g" /etc/nginx/sites-available/gb_service.conf
sed -i "s|/opt/gb_service2022/frontend|$INSTALL_DIR/frontend|g" /etc/nginx/sites-available/gb_service.conf

# 备份默认配置并启用新配置
if [ -f /etc/nginx/sites-enabled/default ]; then
    mv /etc/nginx/sites-enabled/default /etc/nginx/sites-enabled/default.bak.$(date +%Y%m%d%H%M%S)
fi
ln -sf /etc/nginx/sites-available/gb_service.conf /etc/nginx/sites-enabled/

# 测试并重载
nginx -t && systemctl reload nginx
systemctl enable nginx
log_info "Nginx 配置完成"

# ========== 步骤 6: 配置 ZLM Hook ==========
log_step "步骤 6: 配置 ZLMediaKit Hook..."
ZLM_CONFIG="/opt/zlm_media_server/config.ini"

if [ -f "$ZLM_CONFIG" ]; then
    # 备份原配置
    cp "$ZLM_CONFIG" "$ZLM_CONFIG.bak.$(date +%Y%m%d%H%M%S)"
    
    # 检查并更新 hook 配置
    if grep -q "\[hook\]" "$ZLM_CONFIG"; then
        # 更新现有配置
        sed -i "s|on_stream_changed=.*|on_stream_changed=http://127.0.0.1:8080/api/zlm/hook/on_stream_changed|" "$ZLM_CONFIG"
        sed -i "s|on_stream_none_reader=.*|on_stream_none_reader=http://127.0.0.1:8080/api/zlm/hook/on_stream_none_reader|" "$ZLM_CONFIG"
        sed -i "s|on_rtp_server_timeout=.*|on_rtp_server_timeout=http://127.0.0.1:8080/api/zlm/hook/on_rtp_server_timeout|" "$ZLM_CONFIG"
    else
        # 添加 hook 配置段
        cat >> "$ZLM_CONFIG" << EOF

[hook]
on_stream_changed=http://127.0.0.1:8080/api/zlm/hook/on_stream_changed
on_stream_none_reader=http://127.0.0.1:8080/api/zlm/hook/on_stream_none_reader
on_rtp_server_timeout=http://127.0.0.1:8080/api/zlm/hook/on_rtp_server_timeout
EOF
    fi
    
    log_info "ZLM Hook 配置已更新"
    log_warn "请手动重启 ZLMediaKit 服务使配置生效"
else
    log_warn "找不到 ZLM 配置文件: $ZLM_CONFIG"
    log_info "请手动在 ZLM config.ini 中添加以下配置:"
    echo "[hook]"
    echo "on_stream_changed=http://127.0.0.1:8080/api/zlm/hook/on_stream_changed"
    echo "on_stream_none_reader=http://127.0.0.1:8080/api/zlm/hook/on_stream_none_reader"
    echo "on_rtp_server_timeout=http://127.0.0.1:8080/api/zlm/hook/on_rtp_server_timeout"
fi

# 测试 ZLM 连接
if curl -s "http://127.0.0.1:880/index/api/getMediaList?secret=$ZLM_SECRET" > /dev/null 2>&1; then
    log_info "ZLMediaKit 连接正常"
else
    log_warn "无法连接到 ZLMediaKit (http://127.0.0.1:880)"
    log_warn "请确保 ZLM 已启动且 secret 正确"
fi

# ========== 步骤 7: 创建系统服务 ==========
log_step "步骤 7: 创建 systemd 服务..."

# 创建用户（如果不存在）
if ! id "$DB_USER" &>/dev/null; then
    useradd -r -s /bin/false -d $INSTALL_DIR $DB_USER
fi

# 设置权限
chown -R $DB_USER:$DB_USER $INSTALL_DIR
chown $DB_USER:$DB_USER /var/log/gb_service.log

# 复制并更新服务文件
cp ./config/gb_service.service /etc/systemd/system/
sed -i "s|User=.*|User=$DB_USER|" /etc/systemd/system/gb_service.service
sed -i "s|WorkingDirectory=.*|WorkingDirectory=$INSTALL_DIR/backend|" /etc/systemd/system/gb_service.service
sed -i "s|ExecStart=.*|ExecStart=$INSTALL_DIR/backend/gb_service|" /etc/systemd/system/gb_service.service

systemctl daemon-reload
systemctl enable gb_service
log_info "系统服务创建完成"

# ========== 步骤 8: 启动服务 ==========
log_step "步骤 8: 启动国标服务..."
systemctl start gb_service
sleep 3

# 检查状态
if systemctl is-active --quiet gb_service; then
    log_info "gb_service 启动成功"
else
    log_error "gb_service 启动失败"
    journalctl -u gb_service -n 20 --no-pager
    exit 1
fi

# ========== 步骤 9: 验证部署 ==========
log_step "步骤 9: 验证部署..."

# 检查端口
HTTP_OK=0
SIP_OK=0

if netstat -tuln 2>/dev/null | grep -q ":8080"; then
    log_info "HTTP 端口 8080 正常"
    HTTP_OK=1
else
    log_warn "HTTP 端口 8080 未监听"
fi

if netstat -tuln 2>/dev/null | grep -q ":5060"; then
    log_info "SIP 端口 5060 正常"
    SIP_OK=1
else
    log_warn "SIP 端口 5060 未监听"
fi

# 测试 API
if curl -s http://127.0.0.1:8080/api/health > /dev/null 2>&1; then
    log_info "HTTP API 响应正常"
else
    log_warn "HTTP API 未响应，等待 5 秒后重试..."
    sleep 5
    if curl -s http://127.0.0.1:8080/api/health > /dev/null 2>&1; then
        log_info "HTTP API 响应正常"
    else
        log_warn "HTTP API 仍未响应，请检查日志"
    fi
fi

# ========== 部署完成 ==========
echo ""
echo "=============================================="
echo -e "${GREEN}  GB28181 国标服务部署完成${NC}"
echo "=============================================="
echo "  访问地址: http://192.168.1.2/"
echo "  API地址: http://192.168.1.2:8080/"
echo "  SIP端口: 5060 (UDP/TCP)"
echo "  日志文件: /var/log/gb_service.log"
echo "  服务管理: systemctl {start|stop|restart} gb_service"
echo ""
echo "  管理命令:"
echo "    查看状态: sudo systemctl status gb_service"
echo "    查看日志: sudo tail -f /var/log/gb_service.log"
echo "    重启服务: sudo systemctl restart gb_service"
echo "=============================================="
