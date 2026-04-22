#!/bin/bash
set -e

echo "=========================================="
echo "Building gb_service with device cache"
echo "=========================================="

cd /home/user/coder/gb_service2022/backend

# 检查文件是否存在
echo "[1/5] Checking source files..."
for file in src/infra/SipDeviceCache.h src/infra/SipDeviceCache.cpp; do
    if [ -f "$file" ]; then
        echo "  ✓ $file"
    else
        echo "  ✗ $file NOT FOUND"
        exit 1
    fi
done

# 检查 CMakeLists.txt
echo ""
echo "[2/5] Checking CMakeLists.txt..."
if grep -q "SipDeviceCache.cpp" CMakeLists.txt; then
    echo "  ✓ SipDeviceCache.cpp in CMakeLists.txt"
else
    echo "  ⚠ Adding SipDeviceCache.cpp to CMakeLists.txt..."
    sed -i '/SipCatalog.cpp/a\  src/infra/SipDeviceCache.cpp' CMakeLists.txt
fi

# 编译
echo ""
echo "[3/5] Building..."
cd build
cmake .. 2>&1 | tail -5
make -j4 2>&1

if [ -f "gb_service" ]; then
    echo "  ✓ Build successful"
    ls -lh gb_service
else
    echo "  ✗ Build failed"
    exit 1
fi

cd ..

# 数据库迁移
echo ""
echo "[4/5] Database migration..."
if [ -f "db/migration_add_src_addr.sql" ]; then
    echo "  Migration file exists"
    echo "  Please run manually if not done:"
    echo "    sudo -u postgres psql -d gb28181 -f db/migration_add_src_addr.sql"
else
    echo "  Creating migration file..."
    cat > db/migration_add_src_addr.sql << 'EOF'
ALTER TABLE device_platforms
    ADD COLUMN IF NOT EXISTS src_ip VARCHAR(128),
    ADD COLUMN IF NOT EXISTS src_port INTEGER;
EOF
fi

# 总结
echo ""
echo "=========================================="
echo "Build Complete!"
echo "=========================================="
echo "Next steps:"
echo "1. Run: sudo systemctl stop gb_service"
echo "2. Run: sudo -u postgres psql -d gb28181 -f db/migration_add_src_addr.sql"
echo "3. Run: sudo ./build/gb_service"
echo ""
echo "Test cache API:"
echo "  curl http://localhost:8080/api/stats/device-cache"
echo "=========================================="
