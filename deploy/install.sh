#!/bin/bash
# GB28181 国标服务一键部署脚本
# 部署目标: 192.168.1.2
# 版本: 2025-04-21

set -e

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 配置变量
INSTALL_DIR="/opt/gb_service2022"
SERVICE_NAME="gb_service"
HTTP_PORT=8080
SIP_PORT=5060
ZLM_SECRET="sF3auJDv6AqFUs8BfrlLYmDt2H7S9MGC"
DB_NAME="gb28181"
DB_USER="user"
DB_PASS="gb28181_pass"

# 日志函数
log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }
log_step() { echo -e "${BLUE}[STEP]${NC} $1"; }

# 检查 root 权限
check_root() {
    if [ "$EUID" -ne 0 ]; then
        log_error "请使用 root 权限运行此脚本: sudo ./install.sh"
        exit 1
    fi
}

# 检查系统
check_system() {
    log_step "检查系统环境..."
    
    # 检查操作系统
    if [ -f /etc/os-release ]; then
        . /etc/os-release
        log_info "操作系统: $NAME $VERSION_ID"
    fi
    
    # 检查架构
    ARCH=$(uname -m)
    log_info "系统架构: $ARCH"
    
    # 检查必要命令
    for cmd in systemctl apt-get; do
        if ! command -v $cmd &> /dev/null; then
            log_error "缺少命令: $cmd"
            exit 1
        fi
    done
    
    log_info "系统环境检查通过"
}

# 安装依赖
install_dependencies() {
    log_step "安装系统依赖..."
    
    apt-get update
    
    # 安装运行时依赖
    apt-get install -y \
        libpq5 \
        libcurl4 \
        libssl1.1 \
        libuuid1 \
        postgresql \
        postgresql-contrib \
        nginx \
        curl \
        net-tools
    
    # 启动 PostgreSQL
    systemctl enable postgresql
    systemctl start postgresql
    
    log_info "系统依赖安装完成"
}

# 初始化数据库
init_database() {
    log_step "初始化数据库..."
    
    # 创建数据库用户
    sudo -u postgres psql -c "CREATE USER $DB_USER WITH PASSWORD '$DB_PASS';" 2>/dev/null || log_warn "用户可能已存在"
    
    # 创建数据库
    sudo -u postgres psql -c "CREATE DATABASE $DB_NAME OWNER $DB_USER;" 2>/dev/null || log_warn "数据库可能已存在"
    
    # 授予权限
    sudo -u postgres psql -c "GRANT ALL PRIVILEGES ON DATABASE $DB_NAME TO $DB_USER;"
    
    # 导入表结构
    if [ -f "./sql/schema.sql" ]; then
        sudo -u postgres psql -d $DB_NAME -f ./sql/schema.sql
        log_info "数据库表结构导入完成"
    else
        log_error "找不到数据库脚本: ./sql/schema.sql"
        exit 1
    fi
    
    # 初始化配置数据
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
    
    log_info "数据库初始化完成"
}

# 安装应用文件
install_application() {
    log_step "安装应用程序..."
    
    # 创建目录
    mkdir -p $INSTALL_DIR/{backend,frontend/dist,logs,config}
    
    # 复制后端
    cp ./backend/gb_service $INSTALL_DIR/backend/
    chmod +x $INSTALL_DIR/backend/gb_service
    
    # 复制前端
    cp -r ./frontend/dist/* $INSTALL_DIR/frontend/dist/
    
    # 复制配置文件
    cp ./config/gb_service.service /etc/systemd/system/
    cp ./config/gb_service.conf /etc/nginx/sites-available/
    
    # 创建日志文件
    touch /var/log/gb_service.log
    chmod 666 /var/log/gb_service.log
    
    log_info "应用程序安装完成"
}

# 配置 Nginx
configure_nginx() {
    log_step "配置 Nginx..."
    
    # 备份原配置
    if [ -f /etc/nginx/sites-enabled/default ]; then
        mv /etc/nginx/sites-enabled/default /etc/nginx/sites-enabled/default.bak
    fi
    
    # 启用配置
    ln -sf /etc/nginx/sites-available/gb_service.conf /etc/nginx/sites-enabled/
    
    # 更新配置中的路径
    sed -i "s|/home/\*\*\*/coder/gb_service2022|$INSTALL_DIR|g" /etc/nginx/sites-available/gb_service.conf
    
    # 测试配置
    nginx -t
    
    # 重启 Nginx
    systemctl reload nginx
    systemctl enable nginx
    
    log_info "Nginx 配置完成"
}

# 配置服务
configure_service() {
    log_step "配置系统服务..."
    
    # 创建 gb_service 用户（如果不存在）
    if ! id "$DB_USER" &>/dev/null; then
        useradd -r -s /bin/false $DB_USER
    fi
    
    # 设置权限
    chown -R $DB_USER:$DB_USER $INSTALL_DIR
    chown $DB_USER:$DB_USER /var/log/gb_service.log
    
    # 更新服务文件中的用户
    sed -i "s/User=.*/User=$DB_USER/" /etc/systemd/system/gb_service.service
    sed -i "s|WorkingDirectory=.*|WorkingDirectory=$INSTALL_DIR/backend|" /etc/systemd/system/gb_service.service
    sed -i "s|ExecStart=.*|ExecStart=$INSTALL_DIR/backend/gb_service|" /etc/systemd/system/gb_service.service
    
    # 重载 systemd
    systemctl daemon-reload
    systemctl enable gb_service
    
    log_info "服务配置完成"
}

# 检查 ZLM 配置
check_zlm_config() {
    log_step "检查 ZLMediaKit 配置..."
    
    # 检查 ZLM 是否运行
    if curl -s "http://127.0.0.1:880/index/api/getMediaList?secret=$ZLM_SECRET" > /dev/null 2>&1; then
        log_info "ZLMediaKit 运行正常"
    else
        log_warn "无法连接到 ZLMediaKit (http://127.0.0.1:880)"
        log_warn "请确保 ZLM 已启动并配置了正确的 secret"
    fi
    
    # 提示用户配置 ZLM hook
    log_info "请确保 ZLM 配置文件中设置了以下 hook:"
    echo "  on_stream_changed=http://127.0.0.1:8080/api/zlm/hook/on_stream_changed"
    echo "  on_stream_none_reader=http://127.0.0.1:8080/api/zlm/hook/on_stream_none_reader"
    echo "  on_rtp_server_timeout=http://127.0.0.1:8080/api/zlm/hook/on_rtp_server_timeout"
}

# 启动服务
start_services() {
    log_step "启动服务..."
    
    # 启动 gb_service
    systemctl start gb_service
    sleep 3
    
    # 检查状态
    if systemctl is-active --quiet gb_service; then
        log_info "gb_service 启动成功"
    else
        log_error "gb_service 启动失败，请检查日志: journalctl -u gb_service"
        exit 1
    fi
    
    # 检查端口
    if netstat -tuln 2>/dev/null | grep -q ":$HTTP_PORT"; then
        log_info "HTTP 服务正常 (端口 $HTTP_PORT)"
    fi
    
    if netstat -tuln 2>/dev/null | grep -q ":$SIP_PORT"; then
        log_info "SIP 服务正常 (端口 $SIP_PORT)"
    fi
}

# 健康检查
health_check() {
    log_step "执行健康检查..."
    
    # 测试 HTTP API
    if curl -s http://127.0.0.1:$HTTP_PORT/api/health > /dev/null 2>&1; then
        log_info "HTTP API 响应正常"
    else
        log_warn "HTTP API 可能未就绪，等待 5 秒重试..."
        sleep 5
        if curl -s http://127.0.0.1:$HTTP_PORT/api/health > /dev/null 2>&1; then
            log_info "HTTP API 响应正常"
        else
            log_warn "HTTP API 未响应，请检查日志"
        fi
    fi
    
    log_info "部署完成！"
    echo ""
    echo "=============================================="
    echo "  GB28181 国标服务部署成功"
    echo "=============================================="
    echo "  访问地址: http://192.168.1.2/"
    echo "  API地址: http://192.168.1.2:$HTTP_PORT/"
    echo "  SIP端口: $SIP_PORT (UDP/TCP)"
    echo "  日志文件: /var/log/gb_service.log"
    echo "  服务管理: systemctl {start|stop|restart} gb_service"
    echo "=============================================="
}

# 主函数
main() {
    echo "=============================================="
    echo "  GB28181 国标服务一键部署"
    echo "  目标服务器: 192.168.1.2"
    echo "=============================================="
    echo ""
    
    check_root
    check_system
    install_dependencies
    init_database
    install_application
    configure_nginx
    configure_service
    check_zlm_config
    start_services
    health_check
}

# 执行主函数
main "$@"
