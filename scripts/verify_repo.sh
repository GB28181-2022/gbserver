#!/usr/bin/env bash
# 推送/合并后自检：子模块 commit、干净工作区、关键路径存在。
# 成功退出 0；失败退出非 0 并打印原因。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

EXP_PJ="14e2388b8964f1acaa8bf81fda6d213bb4df1507"
EXP_ZL="7302286cf4be39d416b023fec3fd4ca9c54af762"

err() { echo "verify_repo: $*" >&2; exit 1; }

test -d .git || err "not a git repository root"

PJ="$(git -C third_party/pjproject rev-parse HEAD 2>/dev/null)" || err "third_party/pjproject missing or not a git repo"
ZL="$(git -C third_party/ZLToolKit rev-parse HEAD 2>/dev/null)" || err "third_party/ZLToolKit missing or not a git repo"

[[ "$PJ" == "$EXP_PJ" ]] || err "pjproject HEAD mismatch: got $PJ want $EXP_PJ"
[[ "$ZL" == "$EXP_ZL" ]] || err "ZLToolKit HEAD mismatch: got $ZL want $EXP_ZL"

for d in third_party/pjproject third_party/ZLToolKit; do
  S="$(git -C "$d" status --porcelain 2>/dev/null || true)"
  if [[ -n "$S" ]]; then
    echo "$S" >&2
    err "submodule $d is not clean"
  fi
done

test -f third_party/ZLToolKit/CMakeLists.txt || err "ZLToolKit/CMakeLists.txt missing"
test -f third_party/pjproject/configure || err "pjproject/configure missing (incomplete submodule?)"

echo "verify_repo: OK (submodules at pinned commits, clean, key files present)"
echo "  pjproject  $PJ"
echo "  ZLToolKit  $ZL"
