#!/bin/bash
# GB28181 服务全自动部署脚本
# 功能：停止旧服务 -> 编译 -> 数据库迁移 -> 启动服务 -> 健康检查

set -e  # 遇到错误立即退出

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# 日志函数
log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }

# 配置
PROJECT_DIR="/home/user/coder/gb_service2022"
BUILD_DIR="${PROJECT_DIR}/backend/build"
SERVICE_NAME="gb_service"
HTTP_PORT=8080
SIP_PORT=5060
LOG_FILE="/tmp/auto_deploy.log"

# 记录开始时间
START_TIME=$(date +%s)
log_info "=========================================="
log_info "GB28181 服务自动部署开始"
log_info "=========================================="
log_info "时间: $(date '+%Y-%m-%d %H:%M:%S')"
log_info "项目目录: ${PROJECT_DIR}"

# 步骤 1: 停止旧服务
log_info ""
log_info "[步骤 1/7] 停止旧服务..."

# 查找并停止所有 gb_service 进程
OLD_PIDS=$(pgrep -f "${SERVICE_NAME}" || true)
if [ -n "$OLD_PIDS" ]; then
    log_warn "发现旧服务进程: $OLD_PIDS"
    echo "$OLD_PIDS" | xargs -r kill -9 2>/dev/null || true
    sleep 2
    
    # 确认是否还有残留
    REMAINING=$(pgrep -f "${SERVICE_NAME}" || true)
    if [ -n "$REMAINING" ]; then
        log_warn "强制终止残留进程: $REMAINING"
        echo "$REMAINING" | xargs -r sudo kill -9 2>/dev/null || true
        sleep 1
    fi
    log_info "✓ 旧服务已停止"
else
    log_info "✓ 没有运行中的旧服务"
fi

# 释放端口
log_info "检查端口占用..."
for port in $HTTP_PORT $SIP_PORT; do
    PORT_PIDS=$(lsof -ti:$port 2>/dev/null || true)
    if [ -n "$PORT_PIDS" ]; then
        log_warn "端口 $port 被占用，进程: $PORT_PIDS"
        echo "$PORT_PIDS" | xargs -r kill -9 2>/dev/null || true
    fi
done
log_info "✓ 端口已释放"

# 步骤 2: 检查源代码
log_info ""
log_info "[步骤 2/7] 检查源代码..."

cd "${PROJECT_DIR}/backend"

# 检查关键文件
KEY_FILES=(
    "src/infra/SipDeviceCache.h"
    "src/infra/SipDeviceCache.cpp"
    "src/infra/SipHandler.cpp"
    "src/infra/SipHandler.h"
    "src/infra/SipCatalog.cpp"
    "src/infra/SipServerPjsip.cpp"
    "src/main.cpp"
)

for file in "${KEY_FILES[@]}"; do
    if [ ! -f "$file" ]; then
        log_error "缺少关键文件: $file"
        exit 1
    fi
done
log_info "✓ 所有关键文件存在"

# 步骤 3: 编译项目
log_info ""
log_info "[步骤 3/7] 编译项目..."

cd "${BUILD_DIR}"

# 清理旧编译产物（可选）
log_info "配置项目..."
cmake .. > "${LOG_FILE}.cmake" 2>&1
if [ $? -ne 0 ]; then
    log_error "CMake 配置失败"
    tail -20 "${LOG_FILE}.cmake"
    exit 1
fi
log_info "✓ CMake 配置完成"

# 并行编译
log_info "开始编译..."
make -j4 > "${LOG_FILE}.make" 2>&1
if [ $? -ne 0 ]; then
    log_error "编译失败"
    log_error "错误信息:"
    grep -i "error" "${LOG_FILE}.make" | tail -10
    exit 1
fi
log_info "✓ 编译成功"

# 检查可执行文件
if [ ! -f "${SERVICE_NAME}" ]; then
    log_error "找不到可执行文件: ${SERVICE_NAME}"
    exit 1
fi

FILE_SIZE=$(stat -c%s "${SERVICE_NAME}" 2>/dev/null || stat -f%z "${SERVICE_NAME}" 2>/dev/null)
log_info "✓ 可执行文件大小: $(du -h ${SERVICE_NAME} | cut -f1)"

# 步骤 4: 数据库迁移
log_info ""
log_info "[步骤 4/7] 数据库迁移..."

if [ -f "${PROJECT_DIR}/backend/db/migration_add_src_addr.sql" ]; then
    log_info "执行数据库迁移..."
    sudo -u postgres psql -d gb28181 -f "${PROJECT_DIR}/backend/db/migration_add_src_addr.sql" > "${LOG_FILE}.db" 2>&1
    if [ $? -eq 0 ]; then
        log_info "✓ 数据库迁移完成"
    else
        log_warn "数据库迁移可能已执行过或出错（通常可忽略）"
    fi
else
    log_warn "迁移文件不存在，跳过"
fi

# 步骤 5: 启动服务
log_info ""
log_info "[步骤 5/7] 启动服务..."

cd "${BUILD_DIR}"

# 后台启动服务并记录日志
log_info "启动 ${SERVICE_NAME}..."
nohup ./${SERVICE_NAME} > "${LOG_FILE}.service" 2>&1 &
NEW_PID=$!

log_info "服务 PID: $NEW_PID"

# 等待服务启动
log_info "等待服务初始化 (3秒)..."
sleep 3

# 检查进程是否还在运行
if ! kill -0 $NEW_PID 2>/dev/null; then
    log_error "服务启动失败，进程已退出"
    log_error "日志内容:"
    tail -20 "${LOG_FILE}.service"
    exit 1
fi

log_info "✓ 服务进程正在运行"

# 步骤 6: 健康检查
log_info ""
log_info "[步骤 6/7] 健康检查..."

# 检查 HTTP 端口
HTTP_CHECK=0
for i in {1..5}; do
    if curl -s http://localhost:${HTTP_PORT}/ > /dev/null 2>&1; then
        HTTP_CHECK=1
        break
    fi
    log_info "  等待 HTTP 服务... (${i}/5)"
    sleep 1
done

if [ $HTTP_CHECK -eq 1 ]; then
    log_info "✓ HTTP 服务正常 (端口 ${HTTP_PORT})"
else
    log_warn "⚠ HTTP 服务可能未就绪，继续检查 SIP..."
fi

# 检查 SIP 端口
SIP_CHECK=0
for i in {1..3}; do
    if netstat -tuln 2>/dev/null | grep -q ":${SIP_PORT}"; then
        SIP_CHECK=1
        break
    fi
    if ss -tuln 2>/dev/null | grep -q ":${SIP_PORT}"; then
        SIP_CHECK=1
        break
    fi
    sleep 1
done

if [ $SIP_CHECK -eq 1 ]; then
    log_info "✓ SIP 服务正常 (端口 ${SIP_PORT})"
else
    log_warn "⚠ 无法确认 SIP 端口状态"
fi

# 步骤 7: 最终验证
log_info ""
log_info "[步骤 7/7] 最终验证..."

# 检查进程
RUNNING_PIDS=$(pgrep -f "${SERVICE_NAME}" || true)
PID_COUNT=$(echo "$RUNNING_PIDS" | grep -c . || echo "0")

if [ "$PID_COUNT" -ge 1 ]; then
    log_info "✓ 服务运行中，PID: $RUNNING_PIDS"
else
    log_error "✗ 服务未运行"
    exit 1
fi

# 计算耗时
END_TIME=$(date +%s)
DURATION=$((END_TIME - START_TIME))

log_info ""
log_info "=========================================="
log_info "部署完成！"
log_info "=========================================="
log_info "总耗时: ${DURATION} 秒"
log_info "服务 PID: $(pgrep -f ${SERVICE_NAME} | head -1)"
log_info "HTTP 端口: ${HTTP_PORT}"
log_info "SIP 端口: ${SIP_PORT}"
log_info "日志文件: ${LOG_FILE}.service"
log_info ""
log_info "常用命令:"
log_info "  查看日志: tail -f ${LOG_FILE}.service"
log_info "  停止服务: pkill -f ${SERVICE_NAME}"
log_info "  查看状态: curl http://localhost:${HTTP_PORT}/api/stats/device-cache"
log_info "=========================================="

exit 0
