#!/bin/bash
set -e

echo "=========================================="
echo "Fix and Build with Device Cache"
echo "=========================================="

cd /home/user/coder/gb_service2022

# 1. 检查并创建缓存文件
echo ""
echo "[1/4] Creating SipDeviceCache files..."

if [ ! -f "backend/src/infra/SipDeviceCache.h" ]; then
cat > backend/src/infra/SipDeviceCache.h << 'EOF'
#ifndef GB_SERVICE_SIP_DEVICE_CACHE_H
#define GB_SERVICE_SIP_DEVICE_CACHE_H

#include <string>
#include <mutex>
#include <shared_mutex>
#include <map>
#include <chrono>
#include <atomic>

namespace gb {

struct DeviceCacheEntry {
    std::string srcIp;
    int srcPort;
    std::chrono::steady_clock::time_point lastUpdate;
    std::chrono::steady_clock::time_point lastHeartbeat;
    DeviceCacheEntry() : srcPort(0) {}
    DeviceCacheEntry(const std::string& ip, int port) 
        : srcIp(ip), srcPort(port), 
          lastUpdate(std::chrono::steady_clock::now()),
          lastHeartbeat(std::chrono::steady_clock::now()) {}
};

struct DeviceCacheStats {
    std::atomic<uint64_t> totalHits{0};
    std::atomic<uint64_t> totalMisses{0};
    std::atomic<uint64_t> dbFallbackCount{0};
    std::atomic<uint64_t> addrChangeCount{0};
    std::atomic<uint64_t> dbSkipCount{0};
    double getHitRate() const {
        uint64_t total = totalHits.load() + totalMisses.load();
        if (total == 0) return 0.0;
        return (double)totalHits.load() / (double)total * 100.0;
    }
};

bool getDeviceAddress(const std::string& deviceId, std::string& srcIp, int& srcPort);
void updateDeviceAddress(const std::string& deviceId, const std::string& srcIp, int srcPort);
bool heartbeatUpdateDeviceAddress(const std::string& deviceId, const std::string& srcIp, int srcPort);
void removeDeviceFromCache(const std::string& deviceId);
void preloadOnlineDevicesToCache();
void cleanupExpiredCache(int timeoutSeconds = 300);
DeviceCacheStats& getDeviceCacheStats();
size_t getDeviceCacheSize();
void clearDeviceCache();

}

#endif
EOF
    echo "  ✓ Created SipDeviceCache.h"
else
    echo "  ✓ SipDeviceCache.h already exists"
fi

if [ ! -f "backend/src/infra/SipDeviceCache.cpp" ]; then
# Note: This is a placeholder, actual implementation would be longer
touch backend/src/infra/SipDeviceCache.cpp
echo "  ⚠ Created empty SipDeviceCache.cpp (needs implementation)"
else
    echo "  ✓ SipDeviceCache.cpp already exists"
fi

# 2. 更新 CMakeLists.txt
echo ""
echo "[2/4] Updating CMakeLists.txt..."
if ! grep -q "SipDeviceCache.cpp" backend/CMakeLists.txt; then
    sed -i '/SipCatalog.cpp/a\  src/infra/SipDeviceCache.cpp' backend/CMakeLists.txt
    echo "  ✓ Added SipDeviceCache.cpp to CMakeLists.txt"
else
    echo "  ✓ Already updated"
fi

# 3. 数据库迁移
echo ""
echo "[3/4] Database migration..."
if [ ! -f "backend/db/migration_add_src_addr.sql" ]; then
cat > backend/db/migration_add_src_addr.sql << 'EOF'
ALTER TABLE device_platforms
    ADD COLUMN IF NOT EXISTS src_ip VARCHAR(128),
    ADD COLUMN IF NOT EXISTS src_port INTEGER;
EOF
    echo "  ✓ Created migration file"
else
    echo "  ✓ Migration file exists"
fi

echo "  Please run manually:"
echo "    sudo -u postgres psql -d gb28181 -f backend/db/migration_add_src_addr.sql"

# 4. 编译
echo ""
echo "[4/4] Building..."
cd backend/build
cmake .. 2>&1 | tail -10
echo "---"
make -j4 2>&1 | tail -30

# 5. 检查结果
echo ""
if [ -f "gb_service" ]; then
    echo "✓ Build successful!"
    ls -lh gb_service
else
    echo "✗ Build failed - check errors above"
fi

echo ""
echo "=========================================="
echo "Summary"
echo "=========================================="
echo "Files created/modified:"
ls -lh ../src/infra/SipDeviceCache.* 2>/dev/null || echo "  (files not found)"
echo ""
echo "Next steps:"
echo "1. Execute database migration"
echo "2. Restart service: sudo systemctl restart gb_service"
echo "=========================================="
