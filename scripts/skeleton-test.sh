#!/usr/bin/env bash
# 骨架自动化测试：编译后端、构建前端、检查健康接口。需在项目根目录执行。
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

echo "=== 1. 后端编译 ==="
mkdir -p backend/build
cd backend/build
cmake ..
make
cd "$ROOT"
echo "后端编译通过"

echo "=== 2. 后端健康检查（需先启动 gb_service） ==="
if ! curl -sf http://127.0.0.1:8080/api/health | grep -q '"status":"ok"'; then
  echo "请先在另一终端运行: $ROOT/backend/build/gb_service"
  echo "然后重新执行本脚本"
  exit 1
fi
echo "后端 /api/health 返回 ok"

echo "=== 3. 前端构建 ==="
cd "$ROOT/frontend"
npm install
npm run build
cd "$ROOT"
test -f frontend/dist/index.html || { echo "frontend/dist/index.html 不存在"; exit 1; }
echo "前端构建通过"

echo "=== 4. 经 Nginx 访问 /api/health（可选） ==="
if curl -sf http://127.0.0.1/api/health 2>/dev/null | grep -q '"status":"ok"'; then
  echo "Nginx 反代 /api/health 正常"
else
  echo "跳过或未配置 Nginx：请手动访问 http://127.0.0.1/api/health 和 http://127.0.0.1/"
fi

echo "=== 骨架测试完成 ==="
