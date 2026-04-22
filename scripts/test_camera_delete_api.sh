#!/usr/bin/env bash
# 摄像头删除 / 批量删除 HTTP API 自动化测试
# 依赖：后端已监听 BASE_URL（默认 http://127.0.0.1:8080）、本地 psql 可连 gb28181（与 DbUtil 一致）

set -euo pipefail

BASE_URL="${BASE_URL:-http://127.0.0.1:8080}"
PSQL_USER="${PSQL_USER:-user}"
PSQL_DB="${PSQL_DB:-gb28181}"

# 专用测试国标 ID（20 位数字，避免与真实设备冲突）
CAM_SINGLE="99999999999999999991"
CAM_BATCH_A="99999999999999999992"
CAM_BATCH_B="99999999999999999993"

fail() { echo "FAIL: $*" >&2; exit 1; }
ok() { echo "OK   $*"; }

json_code() {
  local body="$1"
  python3 -c "import json,sys; d=json.loads(sys.argv[1]); print(d.get('code',''))" "$body" 2>/dev/null || echo ""
}

need_py() {
  command -v python3 >/dev/null 2>&1 || fail "需要 python3 解析 JSON"
}

http_get() {
  curl -sS "$BASE_URL$1"
}

http_delete() {
  curl -sS -X DELETE "$BASE_URL$1"
}

http_post_json() {
  curl -sS -X POST "$BASE_URL$1" -H "Content-Type: application/json" -d "$2"
}

echo "=== 摄像头删除 API 测试 BASE_URL=$BASE_URL ==="
need_py

# 健康检查
h=$(http_get "/api/health")
[[ $(json_code "$h") == "0" ]] || fail "GET /api/health 不可用: $h"
ok "GET /api/health"

# 404：不存在的摄像头
r=$(http_delete "/api/cameras/${CAM_SINGLE}")
c=$(json_code "$r")
[[ "$c" == "404" ]] || fail "删除不存在摄像头应返回 code=404, got $r"
ok "DELETE 不存在摄像头 -> 404"

# 400：batch 空列表
r=$(http_post_json "/api/cameras/batch-delete" '{"cameraIds":[]}')
c=$(json_code "$r")
[[ "$c" == "400" ]] || fail "空 cameraIds 应 400, got $r"
ok "POST batch-delete 空 cameraIds -> 400"

# 400：超过 100 条
payload=$(python3 - <<'PY'
import json
ids = [f"{i:020d}" for i in range(101)]
print(json.dumps({"cameraIds": ids}))
PY
)
r=$(http_post_json "/api/cameras/batch-delete" "$payload")
c=$(json_code "$r")
[[ "$c" == "400" ]] || fail "101 条应 400, got $r"
ok "POST batch-delete 101 条 -> 400"

# 准备测试数据（单条 + 批量两条）
if ! psql -U "$PSQL_USER" -d "$PSQL_DB" -v ON_ERROR_STOP=1 -c "\
INSERT INTO cameras (id, name) VALUES
  ('${CAM_SINGLE}', 'api-test-single'),
  ('${CAM_BATCH_A}', 'api-test-batch-a'),
  ('${CAM_BATCH_B}', 'api-test-batch-b')
ON CONFLICT (id) DO UPDATE SET name = EXCLUDED.name;\
" >/dev/null 2>&1; then
  echo "WARN: 无法写入测试摄像头（psql 失败），跳过成功路径用例" >&2
  echo "=== 负例已通过，成功路径未执行 ==="
  exit 0
fi
ok "psql 插入测试 cameras 行"

# 单条删除成功
r=$(http_delete "/api/cameras/${CAM_SINGLE}")
c=$(json_code "$r")
[[ "$c" == "0" ]] || fail "DELETE 测试摄像头应成功: $r"
ok "DELETE 单条测试摄像头 -> code=0"

r=$(http_delete "/api/cameras/${CAM_SINGLE}")
c=$(json_code "$r")
[[ "$c" == "404" ]] || fail "再次删除应 404: $r"
ok "DELETE 同一 ID 第二次 -> 404"

# 批量删除
r=$(http_post_json "/api/cameras/batch-delete" "{\"cameraIds\":[\"${CAM_BATCH_A}\",\"${CAM_BATCH_B}\",\"${CAM_SINGLE}\"]}")
c=$(json_code "$r")
[[ "$c" == "0" ]] || fail "batch-delete 应成功: $r"
deleted=$(python3 -c "import json,sys; d=json.loads(sys.argv[1]); print(d.get('data',{}).get('deleted',-1))" "$r")
nf=$(python3 -c "import json,sys; d=json.loads(sys.argv[1]); print(d.get('data',{}).get('notFound',-1))" "$r")
[[ "$deleted" == "2" ]] || fail "应删除 2 条, deleted=$deleted body=$r"
[[ "$nf" == "1" ]] || fail "应 notFound=1（已删的 ${CAM_SINGLE}）, got $nf body=$r"
ok "POST batch-delete -> deleted=2 notFound=1"

# 确认已清空
r=$(http_delete "/api/cameras/${CAM_BATCH_A}")
[[ $(json_code "$r") == "404" ]] || fail "CAM_BATCH_A 应已不存在"
ok "批量删除后 CAM_BATCH_A 404"

echo "=== 全部通过 ==="
