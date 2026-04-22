#!/usr/bin/env bash
# 自检 POST /api/ptz：缺参、非法指令、不存在摄像头（需本地 gb_service + DB）
set -uo pipefail
BASE_URL="${BASE_URL:-http://127.0.0.1:8080}"

# 说明：除 401 外，本服务多数错误仍返回 HTTP 200，业务码见 JSON 字段 code。

echo "== 1) 空 body 或缺字段（期望 JSON code=400）"
curl -sS -o /tmp/ptz_r1.txt -w "http=%{http_code}\n" -X POST "$BASE_URL/api/ptz" \
  -H "Content-Type: application/json" -d '{}' || true
cat /tmp/ptz_r1.txt; echo

echo "== 2) 非法 command（期望 JSON code=400）"
curl -sS -o /tmp/ptz_r2.txt -w "http=%{http_code}\n" -X POST "$BASE_URL/api/ptz" \
  -H "Content-Type: application/json" \
  -d '{"cameraId":"34020000001320000001","platformGbId":"34020000001180000001","command":"nope","action":"start","speed":2}' || true
cat /tmp/ptz_r2.txt; echo

echo "== 3) 不存在的 cameraId（期望 JSON code=404）"
curl -sS -o /tmp/ptz_r3.txt -w "http=%{http_code}\n" -X POST "$BASE_URL/api/ptz" \
  -H "Content-Type: application/json" \
  -d '{"cameraId":"00000000000000000000","platformGbId":"34020000001180000001","command":"up","action":"start","speed":2}' || true
cat /tmp/ptz_r3.txt; echo

echo "== 可选：设置 CAMERA_ID / PLATFORM_GB_ID 测成功路径（平台在线且已入库）"
if [[ -n "${CAMERA_ID:-}" && -n "${PLATFORM_GB_ID:-}" ]]; then
  curl -sS -o /tmp/ptz_r4.txt -w "http=%{http_code}\n" -X POST "$BASE_URL/api/ptz" \
    -H "Content-Type: application/json" \
    -d "{\"cameraId\":\"$CAMERA_ID\",\"platformGbId\":\"$PLATFORM_GB_ID\",\"command\":\"stop\",\"action\":\"stop\",\"speed\":2}"
  cat /tmp/ptz_r4.txt; echo
fi

echo "完成。请结合 gb_service 日志中的 【PTZ】/ DeviceControl 核对 SIP 下发。"
