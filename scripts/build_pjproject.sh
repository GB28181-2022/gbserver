#!/usr/bin/env bash
# 构建 pjproject 并安装到 third_party/pjproject/install，供后端 SIP（PJSIP）使用
# 用法：从项目根目录执行 ./scripts/build_pjproject.sh

set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC_DIR="$ROOT/third_party/pjproject"
INSTALL_DIR="$SRC_DIR/install"

if [[ ! -d "$SRC_DIR" ]]; then
  echo "Cloning pjproject..."
  git clone --depth 1 https://github.com/pjsip/pjproject.git "$SRC_DIR"
fi
cd "$SRC_DIR"

echo "Configuring (prefix=$INSTALL_DIR, CFLAGS=-fPIC)..."
./configure --prefix="$INSTALL_DIR" CFLAGS="-fPIC -O2" \
  --disable-sound --disable-video --disable-opencore-amr \
  --enable-shared=no

echo "Building..."
make dep
make

echo "Installing to $INSTALL_DIR..."
make install

echo "Done. Use in CMake: set(PJSIP_ROOT $INSTALL_DIR)"
ls -la "$INSTALL_DIR/lib" 2>/dev/null || true
ls -la "$INSTALL_DIR/include" 2>/dev/null || true
