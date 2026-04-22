#!/usr/bin/env bash
# SIP REGISTER 探针：验证本机信令端口（UDP / TCP）是否可达，可选 Digest 完整注册。
#
# 用法：
#   ./scripts/test-sip-register.sh [设备ID]
# 环境变量：
#   SIP_HOST          默认 127.0.0.1
#   SIP_PORT          默认 5060
#   SIP_TRANSPORT     udp | tcp | both（默认 both）
#   SIP_RURI          Request-URI，须与首行 REGISTER 一致（默认 sip:3402000000@3402000000）
#   SIP_REALM         401 里的 realm；不设则从 WWW-Authenticate 解析
#   SIP_USER          Digest 用户名（默认与设备 ID 相同）
#   SIP_PASSWORD      若设置（或 AUTO_DIGEST=1），发二次带 Authorization 的 REGISTER，要求 200
#   AUTO_DIGEST=1     从本机 psql gb28181 / gb_local_config 读取 username、password、gb_id、domain 作为鉴权与 R-URI
#   EXPECT_200=1      未配密码时也要求 200（兼容旧行为：无鉴权服务器直接通过）
#   PSQL_USER         AUTO_DIGEST 时 psql 用户，默认 user
#
# 依赖：python3

set -euo pipefail

DEVICE_ID="${1:-34020000003000000001}"
HOST="${SIP_HOST:-127.0.0.1}"
PORT="${SIP_PORT:-5060}"
TRANSPORT="${SIP_TRANSPORT:-both}"
RURI="${SIP_RURI:-sip:3402000000@3402000000}"
SIP_USER="${SIP_USER:-$DEVICE_ID}"
PSQL_USER="${PSQL_USER:-user}"

if ! command -v python3 &>/dev/null; then
  echo "需要 python3" >&2
  exit 1
fi

if [[ "${AUTO_DIGEST:-0}" == "1" ]] && command -v psql &>/dev/null; then
  _line="$(psql -U "$PSQL_USER" -d gb28181 -AtF '|' -c \
    "SELECT COALESCE(TRIM(gb_id),''), COALESCE(TRIM(domain),''), COALESCE(TRIM(username),''), COALESCE(TRIM(password),'') FROM gb_local_config WHERE id=1 LIMIT 1" 2>/dev/null || true)"
  if [[ -n "$_line" ]]; then
    IFS='|' read -r _gb _dom _u _p <<<"$_line"
    if [[ -n "$_gb" && -n "$_dom" ]]; then
      RURI="sip:${_gb}@${_dom}"
    fi
    if [[ -n "$_u" ]]; then
      SIP_USER="$_u"
    fi
    if [[ -n "$_p" ]]; then
      SIP_PASSWORD="$_p"
    fi
  fi
fi

export DEVICE_ID HOST PORT RURI SIP_USER
export SIP_PASSWORD="${SIP_PASSWORD:-}"
export SIP_REALM="${SIP_REALM:-}"
export EXPECT_200="${EXPECT_200:-0}"

echo "=== SIP REGISTER 探针 -> ${HOST}:${PORT} 设备ID=${DEVICE_ID} transport=${TRANSPORT} R-URI=${RURI} ==="
if [[ -n "${SIP_PASSWORD}" ]]; then
  echo "    （已配置密码：将进行 Digest 二次 REGISTER，期望 200）"
elif [[ "${EXPECT_200}" == "1" ]]; then
  echo "    （EXPECT_200=1：期望直接 200，无鉴权或白名单场景）"
else
  echo "    （默认可达性：200 / 401 / 403 均算信令可达）"
fi

FAIL=0

run_py() {
  local proto="$1"
  SIP_PROTO="$proto" python3 << 'PY'
import hashlib, os, re, socket, sys

def md5hex(s: str) -> str:
    return hashlib.md5(s.encode("utf-8")).hexdigest()

def digest_rsp(user: str, realm: str, password: str, method: str, uri: str, nonce: str) -> str:
    ha1 = md5hex(f"{user}:{realm}:{password}")
    ha2 = md5hex(f"{method}:{uri}")
    return md5hex(f"{ha1}:{nonce}:{ha2}")

def parse_digest_params(header_val: str):
    out = {}
    for name in ("realm", "nonce", "opaque", "algorithm"):
        key = name + "="
        i = header_val.find(key)
        if i < 0:
            continue
        i += len(key)
        if i >= len(header_val):
            continue
        if header_val[i] == '"':
            i += 1
            end = header_val.find('"', i)
            if end < 0:
                continue
            out[name] = header_val[i:end]
        else:
            end = i
            while end < len(header_val) and header_val[end] not in ",\r\n":
                end += 1
            out[name] = header_val[i:end].strip()
    return out

def read_first_sip_message(sock) -> str:
    buf = b""
    while b"\r\n\r\n" not in buf and len(buf) < 65536:
        chunk = sock.recv(4096)
        if not chunk:
            break
        buf += chunk
    return buf.decode(errors="replace")

def register_udp(host: str, port: int, msg: bytes) -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", 0))
    client_port = s.getsockname()[1]
    s.settimeout(5)
    # 将 Via 端口改为实际绑定端口
    text = msg.decode()
    text = text.replace("Via: SIP/2.0/UDP 127.0.0.1:9;", f"Via: SIP/2.0/UDP 127.0.0.1:{client_port};")
    s.sendto(text.encode(), (host, port))
    try:
        data, _ = s.recvfrom(65536)
        return data.decode(errors="replace")
    except socket.timeout:
        return "TIMEOUT"
    finally:
        s.close()

def register_tcp(host: str, port: int, msg: bytes) -> str:
    s = socket.create_connection((host, port), timeout=5)
    try:
        s.sendall(msg)
        return read_first_sip_message(s)
    finally:
        s.close()

def build_register(
    ruri: str,
    device_id: str,
    via_transport: str,
    call_id: str,
    cseq: int,
    branch: str,
    extra_headers: str = "",
) -> bytes:
    # Via 里端口占位 9，UDP 路径上会替换为真实端口
    via_tp = "UDP" if via_transport == "udp" else "TCP"
    m = (
        f"REGISTER {ruri} SIP/2.0\r\n"
        f"Via: SIP/2.0/{via_tp} 127.0.0.1:9;branch={branch}\r\n"
        f"From: <sip:{device_id}@{ruri.split('@')[-1]}>;tag=regtag\r\n"
        f"To: <sip:{device_id}@{ruri.split('@')[-1]}>\r\n"
        f"Call-ID: {call_id}\r\n"
        f"CSeq: {cseq} REGISTER\r\n"
        f"Contact: <sip:{device_id}@127.0.0.1:5060>\r\n"
        f"Expires: 7200\r\n"
        f"{extra_headers}"
        f"Content-Length: 0\r\n\r\n"
    )
    return m.encode()

def main():
    proto = os.environ.get("SIP_PROTO", "udp")
    host = os.environ["HOST"]
    port = int(os.environ["PORT"])
    device_id = os.environ["DEVICE_ID"]
    ruri = os.environ["RURI"]
    password = os.environ.get("SIP_PASSWORD", "")
    realm_fix = os.environ.get("SIP_REALM", "")
    expect_200 = os.environ.get("EXPECT_200", "0") == "1"
    sip_user = os.environ.get("SIP_USER", device_id)

    branch1 = "z9hG4bKprobe1"
    call_id = f"probe-{proto}-{os.getpid()}"
    msg1 = build_register(ruri, device_id, proto, call_id, 1, branch1)

    if proto == "tcp":
        r1 = register_tcp(host, port, msg1)
    else:
        r1 = register_udp(host, port, msg1)

    def classify(resp: str):
        if "TIMEOUT" in resp and not resp.lstrip().startswith("SIP/2.0"):
            return "timeout", resp
        if re.search(r"^SIP/2\.0\s+200", resp, re.M):
            return "200", resp
        if re.search(r"^SIP/2\.0\s+401", resp, re.M):
            return "401", resp
        if re.search(r"^SIP/2\.0\s+403", resp, re.M):
            return "403", resp
        if re.search(r"^SIP/2\.0", resp, re.M):
            return "other", resp
        return "bad", resp

    st, _ = classify(r1)

    if password:
        if st != "401":
            print(f"[{proto.upper()}] 首包期望 401 以取 nonce，实际: {st}")
            print(r1[:1200])
            return 1
        wa = None
        for line in r1.split("\r\n"):
            if line.lower().startswith("www-authenticate:"):
                wa = line.split(":", 1)[1].strip()
                break
        if not wa or not wa.lower().startswith("digest"):
            print(f"[{proto.upper()}] 无 WWW-Authenticate Digest")
            print(r1[:1200])
            return 1
        params = parse_digest_params(wa[6:].lstrip())
        realm = realm_fix or params.get("realm", "")
        nonce = params.get("nonce", "")
        opaque = params.get("opaque", "")
        if not realm or not nonce:
            print(f"[{proto.upper()}] 解析 realm/nonce 失败")
            print(r1[:1200])
            return 1
        rsp = digest_rsp(sip_user, realm, password, "REGISTER", ruri, nonce)
        auth = (
            f'Authorization: Digest username="{sip_user}", realm="{realm}", '
            f'nonce="{nonce}", uri="{ruri}", response="{rsp}", algorithm=MD5'
        )
        if opaque:
            auth += f', opaque="{opaque}"'
        auth += "\r\n"
        branch2 = "z9hG4bKprobe2"
        msg2 = build_register(ruri, device_id, proto, call_id, 2, branch2, extra_headers=auth)
        if proto == "tcp":
            r2 = register_tcp(host, port, msg2)
        else:
            r2 = register_udp(host, port, msg2)
        st2, _ = classify(r2)
        if st2 == "200":
            print(f"[{proto.upper()}] Digest REGISTER -> 200 OK")
            return 0
        print(f"[{proto.upper()}] Digest REGISTER 未得 200，实际: {st2}")
        print(r2[:1200])
        return 1

    if expect_200:
        if st == "200":
            print(f"[{proto.upper()}] -> 200 OK")
            return 0
        print(f"[{proto.upper()}] EXPECT_200：实际 {st}")
        print(r1[:1200])
        return 1

    if st in ("200", "401", "403"):
        print(f"[{proto.upper()}] 信令可达 ({st})")
        return 0
    print(f"[{proto.upper()}] 未收到预期 SIP 响应 ({st})")
    print(r1[:1200])
    return 1

if __name__ == "__main__":
    sys.exit(main())
PY
}

case "$TRANSPORT" in
  udp)
    run_py udp || FAIL=1
    ;;
  tcp)
    run_py tcp || FAIL=1
    ;;
  both)
    run_py udp || FAIL=1
    run_py tcp || FAIL=1
    ;;
  *)
    echo "SIP_TRANSPORT 须为 udp、tcp 或 both" >&2
    exit 1
    ;;
esac

if [[ "$FAIL" -ne 0 ]]; then
  echo ""
  echo "=== 探针失败 ==="
  exit 1
fi
echo ""
echo "=== 探针通过 ==="
exit 0
