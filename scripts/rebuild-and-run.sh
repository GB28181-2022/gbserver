#!/usr/bin/env bash
# 编译后端、重启 gb_service、构建前端（开发/自测用）
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

echo "==> cmake build (backend)"
cmake --build "$ROOT/backend/build"

echo "==> ctest (catalog_group_encoding_test)"
(cd "$ROOT/backend/build" && ctest --output-on-failure)

echo "==> stop old gb_service (if any)"
pkill -x gb_service 2>/dev/null || true
sleep 0.5

echo "==> start gb_service (background)"
cd "$ROOT/backend/build"
nohup ./gb_service >> /tmp/gb_service.log 2>&1 &
echo "    pid=$! log=/tmp/gb_service.log"

echo "==> npm run build (frontend)"
cd "$ROOT/frontend"
npm run build

echo "==> done"
