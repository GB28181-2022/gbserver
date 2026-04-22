#!/usr/bin/env bash
# 目录编组 /api/catalog-group/* 功能测试（需已迁移 catalog_group_* 且 gb_service 已启动）
# 用法: BASE=http://127.0.0.1:8080 TOKEN=xxx ./scripts/test_catalog_group.sh
set -euo pipefail
BASE="${BASE:-http://127.0.0.1:8080}"
AUTH=()
if [[ -n "${TOKEN:-}" ]]; then
  AUTH=(-H "Authorization: Bearer ${TOKEN}")
fi

tmp="$(mktemp)"
tmp_body="$(mktemp)"
tmp_orig="$(mktemp)"
trap 'rm -f "$tmp" "$tmp_body" "$tmp_orig"' EXIT

need_py() {
  command -v python3 >/dev/null 2>&1 || {
    echo "FAIL 需要 python3 以运行完整功能测试"
    exit 1
  }
}

echo "== GET /api/health"
code=$(curl -sS -o "$tmp" -w "%{http_code}" "${BASE}/api/health")
body=$(cat "$tmp")
echo "$body"
[[ "$code" == "200" ]] || { echo "FAIL health HTTP $code"; exit 1; }
echo "OK HTTP $code"

echo "== GET /api/catalog-group/nodes?nested=0"
code=$(curl -sS -o "$tmp" -w "%{http_code}" "${AUTH[@]}" "${BASE}/api/catalog-group/nodes?nested=0")
body=$(cat "$tmp")
echo "$body" | head -c 500; echo
[[ "$code" == "200" ]] || { echo "FAIL nodes flat HTTP $code"; exit 1; }
echo "$body" | grep -q '"code":0' || { echo 'FAIL nodes flat code'; exit 1; }
echo "$body" | grep -q '"items"' || { echo 'FAIL nodes flat items'; exit 1; }
echo "OK flat list"

echo "== GET /api/catalog-group/nodes?nested=1 (含 children)"
code=$(curl -sS -o "$tmp" -w "%{http_code}" "${AUTH[@]}" "${BASE}/api/catalog-group/nodes?nested=1")
[[ "$code" == "200" ]] || { echo "FAIL nodes nested HTTP $code"; exit 1; }
grep -q '"children"' "$tmp" || { echo 'FAIL nested 缺少 children 字段'; exit 1; }
echo "OK nested tree"

echo "== GET import-occupancy 缺参 -> HTTP 400"
code=$(curl -sS -o "$tmp" -w "%{http_code}" "${AUTH[@]}" "${BASE}/api/catalog-group/import-occupancy")
[[ "$code" == "400" ]] || { echo "FAIL import-occupancy 缺参 期望 400 实际 $code"; cat "$tmp"; exit 1; }
echo "OK 400"

echo "== GET import-occupancy 非法 platformId -> HTTP 400"
code=$(curl -sS -o "$tmp" -w "%{http_code}" "${AUTH[@]}" "${BASE}/api/catalog-group/import-occupancy?platformId=abc")
[[ "$code" == "400" ]] || { echo "FAIL 期望 400 实际 $code"; exit 1; }
echo "OK 400"

echo "== POST /api/catalog-group/import 空 body -> HTTP 400"
code=$(curl -sS -o "$tmp" -w "%{http_code}" -X POST -H "Content-Type: application/json" \
  "${AUTH[@]}" "${BASE}/api/catalog-group/import" -d '{}')
[[ "$code" == "400" ]] || { echo "FAIL import 空 body 期望 400 实际 $code"; cat "$tmp"; exit 1; }
echo "OK 400"

need_py

echo "== 解析根节点 id"
curl -sS -o "$tmp" "${AUTH[@]}" "${BASE}/api/catalog-group/nodes?nested=0"
root_id=$(python3 -c "import json,sys; d=json.load(sys.stdin); print(d['data']['items'][0]['id'])" <"$tmp")
echo "root_id=$root_id"

echo "== GET .../nodes/{id}/cameras -> 200 且 items"
code=$(curl -sS -o "$tmp" -w "%{http_code}" "${AUTH[@]}" "${BASE}/api/catalog-group/nodes/${root_id}/cameras")
[[ "$code" == "200" ]] || { echo "FAIL cameras HTTP $code"; exit 1; }
grep -q '"code":0' "$tmp" || { echo FAIL; cat "$tmp"; exit 1; }
grep -q '"items"' "$tmp" || { echo FAIL cameras items; exit 1; }
echo "OK cameras list"

echo "== PUT .../cameras 空 cameraIds（全量清空挂载）"
code=$(curl -sS -o "$tmp" -w "%{http_code}" -X PUT -H "Content-Type: application/json" \
  "${AUTH[@]}" "${BASE}/api/catalog-group/nodes/${root_id}/cameras" \
  -d '{"cameraIds":[]}')
[[ "$code" == "200" ]] || { echo "FAIL PUT cameras HTTP $code"; cat "$tmp"; exit 1; }
grep -q '"code":0' "$tmp" || { echo FAIL; exit 1; }
echo "OK PUT cameras []"

echo "== PUT .../nodes/{id} 更新名称（再改回）"
curl -sS -o "$tmp" "${AUTH[@]}" "${BASE}/api/catalog-group/nodes?nested=0"
python3 -c "
import json, sys
d = json.load(open('$tmp', encoding='utf-8'))
rid = '$root_id'
for x in d['data']['items']:
    if str(x['id']) == rid:
        open('$tmp_orig', 'w', encoding='utf-8').write(x['name'])
        break
else:
    print('root id not in list', file=sys.stderr)
    sys.exit(1)
" || { echo "FAIL 备份根节点名称"; exit 1; }
code=$(curl -sS -o "$tmp" -w "%{http_code}" -X PUT -H "Content-Type: application/json" \
  "${AUTH[@]}" "${BASE}/api/catalog-group/nodes/${root_id}" \
  -d '{"name":"_ft_tmp_rename","sortOrder":0,"civilCode":"","businessGroupId":""}')
[[ "$code" == "200" ]] || { echo "FAIL PUT node HTTP $code"; cat "$tmp"; exit 1; }
grep -q '"code":0' "$tmp" || { echo FAIL PUT rename JSON; cat "$tmp"; exit 1; }
python3 -c "
import json
name = open('$tmp_orig', encoding='utf-8').read()
with open('$tmp_body', 'w', encoding='utf-8') as f:
    json.dump({'name': name, 'sortOrder': 0, 'civilCode': '', 'businessGroupId': ''}, f, ensure_ascii=False)
"
code=$(curl -sS -o "$tmp" -w "%{http_code}" -X PUT -H "Content-Type: application/json" \
  "${AUTH[@]}" "${BASE}/api/catalog-group/nodes/${root_id}" -d @"$tmp_body")
[[ "$code" == "200" ]] || { echo "FAIL PUT restore HTTP $code"; cat "$tmp"; exit 1; }
grep -q '"code":0' "$tmp" || { echo FAIL PUT restore JSON; cat "$tmp"; exit 1; }
echo "OK PUT node round-trip"

echo "== GET import-occupancy?platformId=1（存在则 200 + 字段）"
code=$(curl -sS -o "$tmp" -w "%{http_code}" "${AUTH[@]}" "${BASE}/api/catalog-group/import-occupancy?platformId=1")
if [[ "$code" == "200" ]]; then
  grep -q '"sourceGbDeviceIds"' "$tmp" || { echo FAIL occupancy shape; exit 1; }
  grep -q '"cameraIds"' "$tmp" || { echo FAIL occupancy shape; exit 1; }
  echo "OK occupancy platformId=1"
else
  echo "SKIP occupancy platformId=1 HTTP $code（无 id=1 平台时可忽略）"
fi

echo "== POST 子节点 + DELETE"
curl -sS -o "$tmp" "${AUTH[@]}" "${BASE}/api/catalog-group/nodes?nested=0"
parent=$(python3 -c "import json,sys; d=json.load(sys.stdin); print(d['data']['items'][0]['id'])" <"$tmp")
code=$(curl -sS -o "$tmp" -w "%{http_code}" -X POST -H "Content-Type: application/json" \
  "${AUTH[@]}" "${BASE}/api/catalog-group/nodes" \
  -d "{\"parentId\":${parent},\"name\":\"_smoke_catalog_child\",\"nodeType\":1}")
[[ "$code" == "200" ]] || { echo "FAIL POST child HTTP $code"; cat "$tmp"; exit 1; }
grep -q '"code":0' "$tmp" || { echo "FAIL POST child JSON"; cat "$tmp"; exit 1; }
newid=$(python3 -c "import json,sys; print(json.load(sys.stdin)['data']['id'])" <"$tmp")
code=$(curl -sS -o "$tmp" -w "%{http_code}" -X DELETE "${AUTH[@]}" "${BASE}/api/catalog-group/nodes/${newid}")
[[ "$code" == "200" ]] || { echo "FAIL DELETE HTTP $code"; cat "$tmp"; exit 1; }
grep -q '"code":0' "$tmp" || { echo "FAIL DELETE JSON"; exit 1; }
echo "OK CRUD child (id=${newid} removed)"

echo ""
echo "ALL PASSED"
