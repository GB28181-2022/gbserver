/**
 * @file SipDeviceCache.h
 * @brief 设备地址缓存模块
 * @details 管理 SIP 设备（摄像头/平台）的地址信息缓存：
 *          - 缓存设备 IP 和端口，减少数据库查询
 *          - 支持心跳更新缓存
 *          - 自动清理过期缓存
 *          - 提供命中率统计
 * @date 2025
 * @note 线程安全，使用读写锁保护
 */
#ifndef GB_SERVICE_SIP_DEVICE_CACHE_H
#define GB_SERVICE_SIP_DEVICE_CACHE_H

#include <string>
#include <mutex>
#include <shared_mutex>
#include <map>
#include <chrono>
#include <atomic>

namespace gb {

/**
 * @struct DeviceCacheEntry
 * @brief 设备缓存条目
 * @details 存储设备的网络地址和更新时间
 */
struct DeviceCacheEntry {
    std::string srcIp;           /**< 设备 IP 地址 */
    int srcPort;                 /**< 设备端口 */
    std::chrono::steady_clock::time_point lastUpdate;     /**< 最后更新时间 */
    std::chrono::steady_clock::time_point lastHeartbeat;    /**< 最后心跳时间 */
    DeviceCacheEntry() : srcPort(0) {}
    DeviceCacheEntry(const std::string& ip, int port) 
        : srcIp(ip), srcPort(port), 
          lastUpdate(std::chrono::steady_clock::now()),
          lastHeartbeat(std::chrono::steady_clock::now()) {}
};

/**
 * @struct DeviceCacheStats
 * @brief 缓存统计信息
 */
struct DeviceCacheStats {
    std::atomic<uint64_t> totalHits{0};       /**< 缓存命中次数 */
    std::atomic<uint64_t> totalMisses{0};     /**< 缓存未命中次数 */
    std::atomic<uint64_t> dbFallbackCount{0};  /**< 数据库回退查询次数 */
    std::atomic<uint64_t> addrChangeCount{0};  /**< 地址变更次数 */
    std::atomic<uint64_t> dbSkipCount{0};     /**< 跳过数据库查询次数 */
    
    /**
     * @brief 获取缓存命中率
     * @return 命中率百分比（0.0-100.0）
     */
    double getHitRate() const {
        uint64_t total = totalHits.load() + totalMisses.load();
        if (total == 0) return 0.0;
        return (double)totalHits.load() / (double)total * 100.0;
    }
};

/**
 * @brief 获取设备地址
 * @param deviceId 设备 ID
 * @param srcIp 输出：设备 IP
 * @param srcPort 输出：设备端口
 * @return true 获取成功（缓存命中或数据库查询成功）
 */
bool getDeviceAddress(const std::string& deviceId, std::string& srcIp, int& srcPort);

/**
 * @brief 更新设备地址缓存
 * @param deviceId 设备 ID
 * @param srcIp 设备 IP
 * @param srcPort 设备端口
 */
void updateDeviceAddress(const std::string& deviceId, const std::string& srcIp, int srcPort);

/**
 * @brief 心跳更新设备地址
 * @param deviceId 设备 ID
 * @param srcIp 设备 IP
 * @param srcPort 设备端口
 * @return true 地址有变更
 */
bool heartbeatUpdateDeviceAddress(const std::string& deviceId, const std::string& srcIp, int srcPort);

/**
 * @brief 从缓存移除设备
 * @param deviceId 设备 ID
 */
void removeDeviceFromCache(const std::string& deviceId);

/**
 * @brief 预加载在线设备到缓存
 * @details 从数据库加载所有在线设备地址到缓存
 */
void preloadOnlineDevicesToCache();

/**
 * @brief 清理过期缓存
 * @param timeoutSeconds 超时时间（秒），默认 300 秒
 */
void cleanupExpiredCache(int timeoutSeconds = 300);

/**
 * @brief 获取缓存统计信息
 * @return 统计信息引用
 */
DeviceCacheStats& getDeviceCacheStats();

/**
 * @brief 获取缓存大小
 * @return 缓存条目数量
 */
size_t getDeviceCacheSize();

/**
 * @brief 清空缓存
 */
void clearDeviceCache();

}

#endif
