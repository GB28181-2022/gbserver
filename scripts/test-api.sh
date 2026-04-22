#!/usr/bin/env bash
# 新实现接口的 API 测试脚本
# 使用前请确保：1）后端为最新构建并已启动（cd backend/build && ./gb_service）；2）PostgreSQL 已创建 gb_service2022 并执行 schema.sql
# 若新接口返回 404，请重新编译并重启后端后再执行本脚本。
# 用法：./scripts/test-api.sh [BASE_URL]，默认 BASE_URL=http://127.0.0.1:8080

set -e
BASE_URL="${1:-http://127.0.0.1:8080}"
FAIL=0

check() {
  local name="$1"
  local url="$2"
  local want_code="${3:-200}"
  echo -n "  $name ... "
  code=$(curl -s -o /tmp/test_api_body.txt -w "%{http_code}" "$url")
  if [[ "$code" != "$want_code" ]]; then
    echo "FAIL (HTTP $code, expected $want_code)"
    FAIL=1
    return
  fi
  if [[ -f /tmp/test_api_body.txt ]]; then
    code_json=$(grep -o '"code":[0-9]*' /tmp/test_api_body.txt | head -1)
    if [[ -n "$code_json" ]] && ! echo "$code_json" | grep -q '"code":0'; then
      echo "FAIL (response: $(head -c 120 /tmp/test_api_body.txt))"
      FAIL=1
      return
    fi
  fi
  echo "OK"
}

echo "=== API 测试 BASE_URL=$BASE_URL ==="
check "GET /api/health" "$BASE_URL/api/health"
check "GET /api/device-platforms" "$BASE_URL/api/device-platforms"
check "GET /api/device-platforms?page=1&pageSize=2" "$BASE_URL/api/device-platforms?page=1&pageSize=2"
check "GET /api/cameras" "$BASE_URL/api/cameras"
check "GET /api/cameras?page=1&pageSize=2" "$BASE_URL/api/cameras?page=1&pageSize=2"
check "GET /api/catalog/nodes" "$BASE_URL/api/catalog/nodes"
check "GET /api/catalog/nodes/root/cameras" "$BASE_URL/api/catalog/nodes/root/cameras"
check "GET /api/catalog/nodes/1/cameras" "$BASE_URL/api/catalog/nodes/1/cameras"
check "GET /api/alarms" "$BASE_URL/api/alarms"
check "GET /api/overview" "$BASE_URL/api/overview"
check "GET /api/config/media" "$BASE_URL/api/config/media"

# 流媒体 rtpTransport 字段应出现在 GET /api/config/media 中
echo -n "  GET /api/config/media 含 rtpTransport ... "
if ! curl -s "$BASE_URL/api/config/media" | grep -q '"rtpTransport"'; then
  echo "FAIL (missing rtpTransport)"
  FAIL=1
else
  echo "OK"
fi

# 可选：写入测试（PUT 配置、POST 平台），避免污染数据可设 RUN_WRITE_TESTS=0
if [[ "${RUN_WRITE_TESTS:-1}" == "1" ]]; then
  echo "--- 写入接口 ---"
  code=$(curl -s -o /tmp/test_api_body.txt -w "%{http_code}" -X PUT "$BASE_URL/api/config/local-gb" -H "Content-Type: application/json" -d '{"gbId":"34020000002000000001","domain":"3402000000","name":"本级","username":"34020000002000000001","password":"pwd","udp":true,"tcp":false}')
  if [[ "$code" != "200" ]] || ! grep -q '"code":0' /tmp/test_api_body.txt 2>/dev/null; then echo "  PUT /api/config/local-gb ... FAIL (HTTP $code)"; FAIL=1; else echo "  PUT /api/config/local-gb ... OK"; fi
  code=$(curl -s -o /tmp/test_api_body.txt -w "%{http_code}" -X PUT "$BASE_URL/api/config/media" -H "Content-Type: application/json" -d '{"start":30000,"end":30500,"host":"127.0.0.1","port":880,"rtpTransport":"udp"}')
  if [[ "$code" != "200" ]] || ! grep -q '"code":0' /tmp/test_api_body.txt 2>/dev/null; then echo "  PUT /api/config/media ... FAIL (HTTP $code)"; FAIL=1; else echo "  PUT /api/config/media ... OK"; fi
  if ! curl -s "$BASE_URL/api/config/media" | grep -q '"rtpTransport":"udp"'; then echo "  GET /api/config/media rtpTransport=udp ... FAIL"; FAIL=1; else echo "  GET /api/config/media rtpTransport=udp ... OK"; fi
  # 切换为 tcp 需 ZLM setServerConfig 成功；无 ZLM 时可能 502，仅作可选验证
  if [[ "${TEST_MEDIA_TCP_PUT:-0}" == "1" ]]; then
    code=$(curl -s -o /tmp/test_api_body.txt -w "%{http_code}" -X PUT "$BASE_URL/api/config/media" -H "Content-Type: application/json" -d '{"start":30000,"end":30500,"host":"127.0.0.1","port":880,"rtpTransport":"tcp"}')
    if [[ "$code" != "200" ]] || ! grep -q '"code":0' /tmp/test_api_body.txt 2>/dev/null; then echo "  PUT /api/config/media (rtpTransport=tcp) ... SKIP/FAIL (HTTP $code, 需 ZLM)"; [[ "$code" == "502" ]] || FAIL=1; else echo "  PUT /api/config/media (rtpTransport=tcp) ... OK"; fi
  fi
  code=$(curl -s -o /tmp/test_api_body.txt -w "%{http_code}" -X POST "$BASE_URL/api/platforms" -H "Content-Type: application/json" -d '{"name":"测试上级","sipDomain":"3402000000","gbId":"34020000002000000002","sipIp":"192.168.1.2","sipPort":5060,"transport":"udp","regUsername":"u","enabled":true}')
  if [[ "$code" != "200" ]] || ! grep -q '"code":0' /tmp/test_api_body.txt 2>/dev/null; then echo "  POST /api/platforms ... FAIL (HTTP $code)"; FAIL=1; else echo "  POST /api/platforms ... OK"; fi
  code=$(curl -s -o /tmp/test_api_body.txt -w "%{http_code}" -X POST "$BASE_URL/api/catalog/nodes" -H "Content-Type: application/json" -d '{"parentId":"root","name":"测试节点","code":"34","level":0}')
  if [[ "$code" != "200" ]] || ! grep -q '"code":0' /tmp/test_api_body.txt 2>/dev/null; then echo "  POST /api/catalog/nodes ... FAIL (HTTP $code)"; FAIL=1; else echo "  POST /api/catalog/nodes ... OK"; fi
  code=$(curl -s -o /tmp/test_api_body.txt -w "%{http_code}" -X POST "$BASE_URL/api/device-platforms" -H "Content-Type: application/json" -d '{"name":"测试下级平台","gbId":"34020000003000000001","listType":"normal"}')
  if [[ "$code" != "200" ]] || ! grep -q '"code":0' /tmp/test_api_body.txt 2>/dev/null; then echo "  POST /api/device-platforms ... FAIL (HTTP $code)"; FAIL=1; else echo "  POST /api/device-platforms ... OK"; fi
  # PUT 需已知 id：先 GET 取第一条，再 PUT（可选，仅当有数据时）
  id=$(curl -s "$BASE_URL/api/device-platforms?pageSize=1" | grep -o '"id":[0-9]*' | head -1 | cut -d: -f2)
  if [[ -n "$id" ]]; then
    code=$(curl -s -o /tmp/test_api_body.txt -w "%{http_code}" -X PUT "$BASE_URL/api/device-platforms/$id" -H "Content-Type: application/json" -d '{"listType":"whitelist","strategyMode":"inherit"}')
    if [[ "$code" != "200" ]] || ! grep -q '"code":0' /tmp/test_api_body.txt 2>/dev/null; then echo "  PUT /api/device-platforms/$id ... FAIL (HTTP $code)"; FAIL=1; else echo "  PUT /api/device-platforms/$id ... OK"; fi
    code=$(curl -s -o /tmp/test_api_body.txt -w "%{http_code}" -X PUT "$BASE_URL/api/device-platforms/$id" -H "Content-Type: application/json" -d '{"listType":"normal","strategyMode":"custom","customMediaHost":"192.168.1.88","customMediaPort":5060,"streamMediaUrl":"http://10.0.0.1/","streamRtpTransport":"tcp"}')
    if [[ "$code" != "200" ]] || ! grep -q '"code":0' /tmp/test_api_body.txt 2>/dev/null; then echo "  PUT /api/device-platforms/$id (custom+streamRtp) ... FAIL (HTTP $code)"; FAIL=1; else echo "  PUT /api/device-platforms/$id (custom+streamRtp) ... OK"; fi
    if command -v jq >/dev/null 2>&1; then
      rt=$(curl -s "$BASE_URL/api/device-platforms?pageSize=200" | jq -r --argjson i "$id" '.data.items[] | select(.id==$i) | .streamRtpTransport')
      if [[ "$rt" != "tcp" ]]; then echo "  GET device-platforms id=$id streamRtpTransport=tcp ... FAIL (got '$rt')"; FAIL=1; else echo "  GET device-platforms id=$id streamRtpTransport=tcp ... OK"; fi
    else
      curl -s "$BASE_URL/api/device-platforms?pageSize=200" | grep -q '"streamRtpTransport":"tcp"' && echo "  GET device-platforms 含 streamRtpTransport=tcp ... OK" || { echo "  GET device-platforms 含 streamRtpTransport=tcp ... FAIL"; FAIL=1; }
    fi
    code=$(curl -s -o /tmp/test_api_body.txt -w "%{http_code}" -X PUT "$BASE_URL/api/device-platforms/$id" -H "Content-Type: application/json" -d '{"listType":"normal","strategyMode":"custom","customMediaHost":"192.168.1.88","customMediaPort":5060,"streamMediaUrl":"","streamRtpTransport":null}')
    if [[ "$code" != "200" ]] || ! grep -q '"code":0' /tmp/test_api_body.txt 2>/dev/null; then echo "  PUT /api/device-platforms/$id (streamRtp null) ... FAIL (HTTP $code)"; FAIL=1; else echo "  PUT /api/device-platforms/$id (streamRtp null) ... OK"; fi
    code=$(curl -s -o /tmp/test_api_body.txt -w "%{http_code}" -X PUT "$BASE_URL/api/device-platforms/$id" -H "Content-Type: application/json" -d '{"listType":"normal","strategyMode":"inherit"}')
    if [[ "$code" != "200" ]] || ! grep -q '"code":0' /tmp/test_api_body.txt 2>/dev/null; then echo "  PUT /api/device-platforms/$id (restore inherit) ... FAIL (HTTP $code)"; FAIL=1; else echo "  PUT /api/device-platforms/$id (restore inherit) ... OK"; fi
  fi
  # 告警：POST 上报一条，再 PUT 确认
  code=$(curl -s -o /tmp/test_api_body.txt -w "%{http_code}" -X POST "$BASE_URL/api/alarms" -H "Content-Type: application/json" -d '{"channelId":"t1","channelName":"测试通道","level":"major","description":"测试告警"}')
  if [[ "$code" != "200" ]] || ! grep -q '"code":0' /tmp/test_api_body.txt 2>/dev/null; then echo "  POST /api/alarms ... FAIL (HTTP $code)"; FAIL=1; else echo "  POST /api/alarms ... OK"; fi
  aid=$(curl -s "$BASE_URL/api/alarms?pageSize=1" | grep -o '"id":[0-9]*' | head -1 | cut -d: -f2)
  if [[ -n "$aid" ]]; then
    code=$(curl -s -o /tmp/test_api_body.txt -w "%{http_code}" -X PUT "$BASE_URL/api/alarms/$aid" -H "Content-Type: application/json" -d '{"status":"ack"}')
    if [[ "$code" != "200" ]] || ! grep -q '"code":0' /tmp/test_api_body.txt 2>/dev/null; then echo "  PUT /api/alarms/$aid ... FAIL (HTTP $code)"; FAIL=1; else echo "  PUT /api/alarms/$aid ... OK"; fi
  fi
fi

if [[ $FAIL -eq 0 ]]; then
  echo "=== 全部通过 ==="
  exit 0
else
  echo "=== 存在失败项 ==="
  exit 1
fi
