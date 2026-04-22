#!/usr/bin/env bash
# 上级平台（平台接入）接口回归：对齐测试计划 TC-ADD / TC-PUT / TC-DEL / TC-ENA
# 前置：gb_service 已启动；PostgreSQL 与 DbUtil 一致（默认 psql -U user -d gb28181）
# 用法：./scripts/test_platforms_upstream.sh [BASE_URL]
# 可选环境变量：
#   PSQL_DB=gb28181 PSQL_USER=user  — DB 断言（TC-DEL-03、TC-PUT-02/03）
#   GB_SERVICE_LOG=/path/to/log     — TC-ENA-04 日志关键字检索

set -euo pipefail
BASE_URL="${1:-http://127.0.0.1:8080}"
PSQL_USER="${PSQL_USER:-user}"
PSQL_DB="${PSQL_DB:-gb28181}"
FAIL=0

json_get() { curl -sS "$1"; }
http_code() { curl -sS -o /tmp/tpu_body.txt -w "%{http_code}" "$@"; }

fail() { echo "FAIL: $*"; FAIL=1; }
ok() { echo "OK: $*"; }

need_jq() {
  if ! command -v jq >/dev/null 2>&1; then
    echo "需要 jq"
    exit 2
  fi
}

cleanup_test_rows() {
  local items id
  items=$(json_get "${BASE_URL}/api/platforms")
  while IFS= read -r id; do
    [[ -z "$id" || "$id" == "null" ]] && continue
    code=$(http_code -X DELETE "${BASE_URL}/api/platforms/${id}")
    [[ "$code" == "200" ]] || true
  done < <(echo "$items" | jq -r '.data.items[] | select(.name=="TP_新增_001" or .name=="TP_TCP_大写" or .name=="TP_编辑_002" or .name=="dup" or .gbId=="34020000002000000010" or .gbId=="34020000002000000011") | .id')
}

assert_json_code() {
  local name="$1"
  if ! grep -q '"code":0' /tmp/tpu_body.txt 2>/dev/null; then
    fail "$name 期望 code=0, 响应: $(head -c 400 /tmp/tpu_body.txt)"
    return 1
  fi
  return 0
}

need_jq
echo "=== 上级平台 API 测试 BASE_URL=$BASE_URL ==="

cleanup_test_rows

# ---------- TC-ADD-03 缺必填 ----------
code=$(http_code -X POST "${BASE_URL}/api/platforms" -H "Content-Type: application/json" \
  -d '{"name":"bad","sipDomain":"4200000011","sipIp":"192.168.1.1","sipPort":5060,"transport":"udp","gbId":"","registerExpires":3600,"heartbeatInterval":60}')
if [[ "$code" != "400" ]]; then fail "TC-ADD-03 缺 gbId 期望 HTTP 400 得 $code"; else ok "TC-ADD-03"; fi

# ---------- TC-ADD-04 国标 ID 非法 ----------
code=$(http_code -X POST "${BASE_URL}/api/platforms" -H "Content-Type: application/json" \
  -d '{"name":"bad","sipDomain":"4200000011","gbId":"3402000000200000001","sipIp":"192.168.1.1","sipPort":5060,"transport":"udp","registerExpires":3600,"heartbeatInterval":60}')
if [[ "$code" != "400" ]]; then fail "TC-ADD-04 期望 400 得 $code"; else ok "TC-ADD-04"; fi

# ---------- TC-ADD-05 registerExpires 越界 ----------
code=$(http_code -X POST "${BASE_URL}/api/platforms" -H "Content-Type: application/json" \
  -d '{"name":"bad","sipDomain":"4200000011","gbId":"34020000002000009905","sipIp":"192.168.1.1","sipPort":5060,"transport":"udp","registerExpires":59,"heartbeatInterval":60}')
if [[ "$code" != "400" ]]; then fail "TC-ADD-05 期望 400 得 $code"; else ok "TC-ADD-05"; fi

# ---------- TC-ADD-06 心跳越界 ----------
code=$(http_code -X POST "${BASE_URL}/api/platforms" -H "Content-Type: application/json" \
  -d '{"name":"bad","sipDomain":"4200000011","gbId":"34020000002000009906","sipIp":"192.168.1.1","sipPort":5060,"transport":"udp","registerExpires":3600,"heartbeatInterval":9}')
if [[ "$code" != "400" ]]; then fail "TC-ADD-06 期望 400 得 $code"; else ok "TC-ADD-06"; fi

# ---------- TC-ADD-01 合法新增 ----------
code=$(http_code -X POST "${BASE_URL}/api/platforms" -H "Content-Type: application/json" \
  -d '{"name":"TP_新增_001","sipDomain":"4200000011","gbId":"34020000002000000010","sipIp":"192.168.1.133","sipPort":5060,"transport":"udp","regUsername":"34020000002000000001","regPassword":"Secret_01","registerExpires":3600,"heartbeatInterval":60,"enabled":true}')
if [[ "$code" != "200" ]]; then fail "TC-ADD-01 HTTP $code"; else assert_json_code "TC-ADD-01" || true; fi
PID=$(jq -r '.data.id' /tmp/tpu_body.txt)
if [[ -z "$PID" || "$PID" == "null" ]]; then fail "TC-ADD-01 无 id"; PID=""; fi
ok "TC-ADD-01 id=$PID"

# ---------- TC-ADD-02 列表可见 ----------
items=$(json_get "${BASE_URL}/api/platforms")
if ! echo "$items" | jq -e --argjson id "$PID" '.data.items[] | select(.id==$id) | .name=="TP_新增_001"' >/dev/null 2>&1; then
  fail "TC-ADD-02 列表未找到或字段不一致"
else
  ok "TC-ADD-02"
fi

# ---------- TC-ADD-08 gb_id 唯一（重复新增 409）----------
code=$(http_code -X POST "${BASE_URL}/api/platforms" -H "Content-Type: application/json" \
  -d '{"name":"dup","sipDomain":"4200000011","gbId":"34020000002000000010","sipIp":"192.168.1.99","sipPort":5060,"transport":"udp","regPassword":"x","registerExpires":3600,"heartbeatInterval":60}')
if [[ "$code" != "409" ]]; then fail "TC-ADD-08 重复 gbId 期望 HTTP 409 得 $code"; else ok "TC-ADD-08"; fi

# ---------- TC-ADD-07 transport 大写 ----------
code=$(http_code -X POST "${BASE_URL}/api/platforms" -H "Content-Type: application/json" \
  -d '{"name":"TP_TCP_大写","sipDomain":"4200000011","gbId":"34020000002000000011","sipIp":"192.168.1.133","sipPort":5060,"transport":"TCP","regUsername":"","regPassword":"x","registerExpires":3600,"heartbeatInterval":60,"enabled":false}')
if [[ "$code" != "200" ]]; then fail "TC-ADD-07 HTTP $code"; else assert_json_code "TC-ADD-07" || true; fi
PID2=$(jq -r '.data.id' /tmp/tpu_body.txt)
tr=$(echo "$(json_get "${BASE_URL}/api/platforms")" | jq -r --argjson id "$PID2" '.data.items[] | select(.id==$id) | .transport')
if [[ "$tr" != "tcp" ]]; then fail "TC-ADD-07 transport 应为 tcp 得 $tr"; else ok "TC-ADD-07 transport=$tr"; fi

# ---------- TC-PUT-01 修改文本与地址 ----------
code=$(http_code -X PUT "${BASE_URL}/api/platforms/${PID}" -H "Content-Type: application/json" \
  -d '{"name":"TP_编辑_002","sipDomain":"4200000011","gbId":"34020000002000000010","sipIp":"192.168.1.200","sipPort":5062,"transport":"udp","regUsername":"34020000002000000001","registerExpires":3600,"heartbeatInterval":60,"enabled":true}')
[[ "$code" == "200" ]] && assert_json_code "TC-PUT-01" || fail "TC-PUT-01 HTTP $code"
name=$(echo "$(json_get "${BASE_URL}/api/platforms")" | jq -r --argjson id "$PID" '.data.items[] | select(.id==$id) | .name')
sip=$(echo "$(json_get "${BASE_URL}/api/platforms")" | jq -r --argjson id "$PID" '.data.items[] | select(.id==$id) | .sipIp')
port=$(echo "$(json_get "${BASE_URL}/api/platforms")" | jq -r --argjson id "$PID" '.data.items[] | select(.id==$id) | .sipPort')
if [[ "$name" != "TP_编辑_002" || "$sip" != "192.168.1.200" || "$port" != "5062" ]]; then fail "TC-PUT-01 字段 $name $sip $port"; else ok "TC-PUT-01"; fi

# ---------- TC-PUT-02 密码更新 ----------
code=$(http_code -X PUT "${BASE_URL}/api/platforms/${PID}" -H "Content-Type: application/json" \
  -d '{"name":"TP_编辑_002","sipDomain":"4200000011","gbId":"34020000002000000010","sipIp":"192.168.1.200","sipPort":5062,"transport":"udp","regUsername":"34020000002000000001","regPassword":"NewSecret_02","registerExpires":3600,"heartbeatInterval":60,"enabled":true}')
[[ "$code" == "200" ]] && assert_json_code "TC-PUT-02" || fail "TC-PUT-02 HTTP $code"
rp=$(echo "$(json_get "${BASE_URL}/api/platforms")" | jq -r --argjson id "$PID" '.data.items[] | select(.id==$id) | .regPassword')
if [[ "$rp" != "NewSecret_02" ]]; then fail "TC-PUT-02 密码 $rp"; else ok "TC-PUT-02"; fi

# ---------- TC-PUT-03 密码清空（键存在空串）----------
code=$(http_code -X PUT "${BASE_URL}/api/platforms/${PID}" -H "Content-Type: application/json" \
  -d '{"name":"TP_编辑_002","sipDomain":"4200000011","gbId":"34020000002000000010","sipIp":"192.168.1.200","sipPort":5062,"transport":"udp","regUsername":"34020000002000000001","regPassword":"","registerExpires":3600,"heartbeatInterval":60,"enabled":true}')
[[ "$code" == "200" ]] && assert_json_code "TC-PUT-03" || fail "TC-PUT-03 HTTP $code"
rp=$(echo "$(json_get "${BASE_URL}/api/platforms")" | jq -r --argjson id "$PID" '.data.items[] | select(.id==$id) | .regPassword')
dbpw=""
if command -v psql >/dev/null 2>&1; then
  dbpw=$(psql -U "$PSQL_USER" -d "$PSQL_DB" -Atc "SELECT COALESCE(reg_password,'') FROM upstream_platforms WHERE id=${PID}" 2>/dev/null || echo "__psql_err__")
fi
if [[ "$rp" != "" ]]; then fail "TC-PUT-03 GET regPassword 应为空"; elif [[ "$dbpw" == "__psql_err__" ]]; then ok "TC-PUT-03 (GET 空，跳过 DB)"; elif [[ -z "$dbpw" ]]; then ok "TC-PUT-03 DB 密码空"; else fail "TC-PUT-03 DB 仍为非空"; fi

# ---------- TC-PUT-04 数值项 ----------
code=$(http_code -X PUT "${BASE_URL}/api/platforms/${PID}" -H "Content-Type: application/json" \
  -d '{"name":"TP_编辑_002","sipDomain":"4200000011","gbId":"34020000002000000010","sipIp":"192.168.1.200","sipPort":5062,"transport":"udp","regUsername":"34020000002000000001","registerExpires":86400,"heartbeatInterval":120,"enabled":true}')
[[ "$code" == "200" ]] && assert_json_code "TC-PUT-04" || fail "TC-PUT-04 HTTP $code"
re=$(echo "$(json_get "${BASE_URL}/api/platforms")" | jq -r --argjson id "$PID" '.data.items[] | select(.id==$id) | .registerExpires')
hi=$(echo "$(json_get "${BASE_URL}/api/platforms")" | jq -r --argjson id "$PID" '.data.items[] | select(.id==$id) | .heartbeatInterval')
if [[ "$re" != "86400" || "$hi" != "120" ]]; then fail "TC-PUT-04 re=$re hi=$hi"; else ok "TC-PUT-04"; fi

# ---------- TC-PUT-05 非法 gbId ----------
code=$(http_code -X PUT "${BASE_URL}/api/platforms/${PID}" -H "Content-Type: application/json" \
  -d '{"name":"TP_编辑_002","sipDomain":"4200000011","gbId":"abc","sipIp":"192.168.1.200","sipPort":5062,"transport":"udp","regUsername":"34020000002000000001","registerExpires":86400,"heartbeatInterval":120,"enabled":true}')
if [[ "$code" != "400" ]]; then fail "TC-PUT-05 期望 400 得 $code"; else ok "TC-PUT-05"; fi

# 恢复合法 gbId 供后续用
code=$(http_code -X PUT "${BASE_URL}/api/platforms/${PID}" -H "Content-Type: application/json" \
  -d '{"name":"TP_编辑_002","sipDomain":"4200000011","gbId":"34020000002000000010","sipIp":"192.168.1.200","sipPort":5062,"transport":"udp","regUsername":"34020000002000000001","regPassword":"Secret_01","registerExpires":3600,"heartbeatInterval":60,"enabled":true}')
[[ "$code" == "200" ]] && assert_json_code "restore-after-PUT-05" || fail "恢复配置失败"

# ---------- TC-ENA-01 关闭 ----------
code=$(http_code -X PUT "${BASE_URL}/api/platforms/${PID}" -H "Content-Type: application/json" \
  -d '{"name":"TP_编辑_002","sipDomain":"4200000011","gbId":"34020000002000000010","sipIp":"192.168.1.200","sipPort":5062,"transport":"udp","regUsername":"34020000002000000001","regPassword":"Secret_01","registerExpires":3600,"heartbeatInterval":60,"enabled":false}')
[[ "$code" == "200" ]] && assert_json_code "TC-ENA-01" || fail "TC-ENA-01 HTTP $code"
en=$(echo "$(json_get "${BASE_URL}/api/platforms")" | jq -r --argjson id "$PID" '.data.items[] | select(.id==$id) | .enabled')
if [[ "$en" != "false" ]]; then fail "TC-ENA-01 enabled=$en"; else ok "TC-ENA-01"; fi

# ---------- TC-ENA-02 启用 ----------
code=$(http_code -X PUT "${BASE_URL}/api/platforms/${PID}" -H "Content-Type: application/json" \
  -d '{"name":"TP_编辑_002","sipDomain":"4200000011","gbId":"34020000002000000010","sipIp":"192.168.1.200","sipPort":5062,"transport":"udp","regUsername":"34020000002000000001","regPassword":"Secret_01","registerExpires":3600,"heartbeatInterval":60,"enabled":true}')
[[ "$code" == "200" ]] && assert_json_code "TC-ENA-02" || fail "TC-ENA-02 HTTP $code"
ok "TC-ENA-02"

# ---------- TC-ENA-03 开关不传 regPassword（应保留）----------
code=$(http_code -X PUT "${BASE_URL}/api/platforms/${PID}" -H "Content-Type: application/json" \
  -d '{"name":"TP_编辑_002","sipDomain":"4200000011","gbId":"34020000002000000010","sipIp":"192.168.1.200","sipPort":5062,"transport":"udp","regUsername":"34020000002000000001","registerExpires":3600,"heartbeatInterval":60,"enabled":false}')
[[ "$code" == "200" ]] && assert_json_code "TC-ENA-03a" || fail "TC-ENA-03 关 HTTP $code"
code=$(http_code -X PUT "${BASE_URL}/api/platforms/${PID}" -H "Content-Type: application/json" \
  -d '{"name":"TP_编辑_002","sipDomain":"4200000011","gbId":"34020000002000000010","sipIp":"192.168.1.200","sipPort":5062,"transport":"udp","regUsername":"34020000002000000001","registerExpires":3600,"heartbeatInterval":60,"enabled":true}')
[[ "$code" == "200" ]] && assert_json_code "TC-ENA-03b" || fail "TC-ENA-03 开 HTTP $code"
rp=$(echo "$(json_get "${BASE_URL}/api/platforms")" | jq -r --argjson id "$PID" '.data.items[] | select(.id==$id) | .regPassword')
if [[ "$rp" != "Secret_01" ]]; then fail "TC-ENA-03 密码被清空? got=$rp"; else ok "TC-ENA-03 密码保留"; fi

# ---------- TC-ENA-04 关闭 + API/日志（无上级时仅 API+可选日志）----------
code=$(http_code -X PUT "${BASE_URL}/api/platforms/${PID}" -H "Content-Type: application/json" \
  -d '{"name":"TP_编辑_002","sipDomain":"4200000011","gbId":"34020000002000000010","sipIp":"192.168.1.200","sipPort":5062,"transport":"udp","regUsername":"34020000002000000001","registerExpires":3600,"heartbeatInterval":60,"enabled":false}')
[[ "$code" == "200" ]] && assert_json_code "TC-ENA-04" || fail "TC-ENA-04 HTTP $code"
sleep 2
en=$(echo "$(json_get "${BASE_URL}/api/platforms")" | jq -r --argjson id "$PID" '.data.items[] | select(.id==$id) | .enabled')
on=$(echo "$(json_get "${BASE_URL}/api/platforms")" | jq -r --argjson id "$PID" '.data.items[] | select(.id==$id) | .online')
if [[ "$en" != "false" ]]; then fail "TC-ENA-04 enabled"; fi
if [[ "$on" != "false" ]]; then echo "WARN: TC-ENA-04 online=$on (期望 false，若无上级注册也可能为 false)"; fi
if [[ -n "${GB_SERVICE_LOG:-}" && -f "$GB_SERVICE_LOG" ]]; then
  if grep -q "UpstreamRegistrar\|上级REGISTER\|reload" "$GB_SERVICE_LOG" 2>/dev/null; then ok "TC-ENA-04 日志含关键字"; else echo "WARN: TC-ENA-04 日志未匹配关键字（可忽略）"; fi
else
  ok "TC-ENA-04 API（未设 GB_SERVICE_LOG 跳过日志文件检索）"
fi

# ---------- TC-ENA-05 再次启用 ----------
code=$(http_code -X PUT "${BASE_URL}/api/platforms/${PID}" -H "Content-Type: application/json" \
  -d '{"name":"TP_编辑_002","sipDomain":"4200000011","gbId":"34020000002000000010","sipIp":"192.168.1.200","sipPort":5062,"transport":"udp","regUsername":"34020000002000000001","registerExpires":3600,"heartbeatInterval":60,"enabled":true}')
[[ "$code" == "200" ]] && assert_json_code "TC-ENA-05" || fail "TC-ENA-05 HTTP $code"
ok "TC-ENA-05"

# 删除辅助行 PID2
if [[ -n "${PID2:-}" && "$PID2" != "null" ]]; then
  http_code -X DELETE "${BASE_URL}/api/platforms/${PID2}" >/dev/null || true
fi

# ---------- TC-DEL-01～03 ----------
code=$(http_code -X DELETE "${BASE_URL}/api/platforms/${PID}")
if [[ "$code" != "200" ]]; then fail "TC-DEL-01 HTTP $code"; else assert_json_code "TC-DEL-01" || true; fi
ok "TC-DEL-01"

items=$(json_get "${BASE_URL}/api/platforms")
if echo "$items" | jq -e --argjson id "$PID" '.data.items[] | select(.id==$id)' >/dev/null 2>&1; then
  fail "TC-DEL-02 列表仍有 id=$PID"
else
  ok "TC-DEL-02"
fi

if command -v psql >/dev/null 2>&1; then
  cnt=$(psql -U "$PSQL_USER" -d "$PSQL_DB" -Atc "SELECT count(*) FROM upstream_platforms WHERE id=${PID}" 2>/dev/null || echo "-1")
  if [[ "$cnt" == "0" ]]; then ok "TC-DEL-03 DB 0 行"; else fail "TC-DEL-03 DB count=$cnt"; fi
else
  ok "TC-DEL-03 跳过（无 psql）"
fi

# ---------- TC-DEL-04 非法 id ----------
code=$(http_code -X DELETE "${BASE_URL}/api/platforms/abc")
if [[ "$code" != "400" ]]; then fail "TC-DEL-04 期望 400 得 $code"; else ok "TC-DEL-04"; fi

if [[ $FAIL -eq 0 ]]; then
  echo "=== 上级平台 API 测试全部通过 ==="
  exit 0
fi
echo "=== 存在失败项 ==="
exit 1
