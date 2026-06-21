#pragma once

#include <chronos/core/types.hpp>
#include <array>
#include <atomic>
#include <optional>
#include <utility>
#include <vector>
#include <chrono>
#include <cstring>

namespace chronos {
namespace market_data {

/**
 * @brief Ultra-high-performance OrderBook with 5-level hot data and double buffering
 * 
 * Performance optimizations:
 * - Hot data: Top 5 levels in 256-byte cache-aligned structure (4 cache lines)
 * - Cold data: Full 20 levels using double buffering with atomic pointer swap
 * - Lock-free reads via optimistic versioning (hot) and RCU-style pointer swap (cold)
 * - Zero-copy reads for strategy threads
 * 
 * Latency targets:
 * - Hot path (top 5 levels): <10ns
 * - Cold path (full 20 levels): <50ns
 * - Update latency: <100ns
 */
class OrderBookV2 {
public:
    OrderBookV2();
    ~OrderBookV2();

    OrderBookV2(const OrderBookV2&) = delete;
    OrderBookV2& operator=(const OrderBookV2&) = delete;

    void update(const Tick& tick);
    std::optional<Decimal> getBestBid() const;
    std::optional<Decimal> getBestAsk() const;
    std::optional<Decimal> getMidPrice() const;
    std::optional<Decimal> getSpread() const;
    std::array<PriceLevel, 5> getTop5Bids() const;
    std::array<PriceLevel, 5> getTop5Asks() const;
    std::optional<PriceLevel> getBidLevel(size_t level) const;
    std::optional<PriceLevel> getAskLevel(size_t level) const;
    std::vector<PriceLevel> getBidLevels(size_t max_levels = 20) const;
    std::vector<PriceLevel> getAskLevels(size_t max_levels = 20) const;
    std::pair<std::array<PriceLevel, 20>, uint8_t> getBidLevelsFast() const;
    std::pair<std::array<PriceLevel, 20>, uint8_t> getAskLevelsFast() const;
    OrderBookSnapshot generateSnapshot() const;
    void clear();
    bool empty() const;
    uint64_t getLastUpdateTime() const;
    struct Statistics {
        uint64_t total_updates = 0;
        uint64_t hot_path_reads = 0;
        uint64_t cold_path_reads = 0;
        uint64_t buffer_swaps = 0;
        uint64_t version_retries = 0;
        std::chrono::nanoseconds avg_update_latency{0};
        std::chrono::nanoseconds avg_hot_read_latency{0};
        std::chrono::nanoseconds avg_cold_read_latency{0};
    };

    Statistics getStatistics() const;

private:
    // Maximum depth to maintain
    static constexpr size_t MAX_DEPTH = 20;
    static constexpr size_t HOT_DEPTH = 5;

    /**
     * @brief Hot data: Top 5 levels in cache-aligned structure
     * 
     * Layout (256 bytes = 4 cache lines):
     * - Bid prices and quantities: 5 * 8 * 2 = 80 bytes
     * - Ask prices and quantities: 5 * 8 * 2 = 80 bytes
     * - Metadata: version, timestamp, counts = 18 bytes
     * - Padding: 78 bytes to reach 256 bytes
     * 
     * Total: 256 bytes aligned to 64-byte cache line boundary
     * 
     * Note: Decimal is 8 bytes (64-bit fixed-point)
     */
    struct alignas(256) HotData {
        // Bid levels (highest price first)
        Decimal bid_prices[HOT_DEPTH];      // 40 bytes
        Decimal bid_quantities[HOT_DEPTH];  // 40 bytes
        
        // Ask levels (lowest price first)
        Decimal ask_prices[HOT_DEPTH];      // 40 bytes
        Decimal ask_quantities[HOT_DEPTH];  // 40 bytes
        
        // Metadata
        std::atomic<uint64_t> version;     // 8 bytes - for optimistic reads
        uint64_t last_update_time;         // 8 bytes
        uint8_t bid_count;                 // 1 byte - number of valid bid levels (0-5)
        uint8_t ask_count;                 // 1 byte - number of valid ask levels (0-5)
        
        // Padding to reach 256 bytes (4 cache lines)
        // 40 + 40 + 40 + 40 + 8 + 8 + 1 + 1 = 178 bytes
        // Need 256 - 178 = 78 bytes padding
        uint8_t padding[78];
        
        HotData() : version(0), last_update_time(0), bid_count(0), ask_count(0) {
            std::memset(bid_prices, 0, sizeof(bid_prices));
            std::memset(bid_quantities, 0, sizeof(bid_quantities));
            std::memset(ask_prices, 0, sizeof(ask_prices));
            std::memset(ask_quantities, 0, sizeof(ask_quantities));
            std::memset(padding, 0, sizeof(padding));
        }
    };
    
    static_assert(sizeof(HotData) == 256, "HotData must be exactly 256 bytes");
    static_assert(alignof(HotData) == 256, "HotData must be 256-byte aligned");

    /**
     * @brief Cold data: Full 20 levels using fixed-size arrays
     * 
     * Double buffering strategy:
     * - Two complete copies of 20-level depth data
     * - Writer updates the "dark" buffer
     * - Atomic pointer swap makes it the "light" buffer
     * - Readers always access via atomic pointer (lock-free)
     */
    struct ColdData {
        // Fixed-size arrays for predictable memory layout
        struct Level {
            Decimal price;
            Decimal quantity;
            
            Level() : price(0), quantity(0) {}
            Level(Decimal p, Decimal q) : price(p), quantity(q) {}
            
            bool is_valid() const { return quantity > Decimal(0); }
        };
        
        Level bids[MAX_DEPTH];  // Sorted: highest price first
        Level asks[MAX_DEPTH];  // Sorted: lowest price first
        
        uint8_t bid_count;      // Number of valid bid levels (0-20)
        uint8_t ask_count;      // Number of valid ask levels (0-20)
        uint64_t last_update_time;
        
        ColdData() : bid_count(0), ask_count(0), last_update_time(0) {}
        
        void clear() {
            bid_count = 0;
            ask_count = 0;
            for (size_t i = 0; i < MAX_DEPTH; ++i) {
                bids[i] = Level();
                asks[i] = Level();
            }
        }
    };

    // Hot data (mutable for optimistic reads)
    mutable HotData hot_data_;

    // Double buffering for cold data
    ColdData cold_buffers_[2];
    std::atomic<ColdData*> active_cold_buffer_;  // Points to current "light" buffer
    size_t write_buffer_index_;                   // Index of "dark" buffer (0 or 1)

    // Statistics (deliberately unprotected to maintain lock-free properties)
    mutable Statistics stats_;

    // Helper methods
    void updateLevels(const Tick& tick);
    void rebuildHotData();
    void swapColdBuffers();
    
    // Optimistic read for hot data
    template<typename Func>
    auto readHotDataOptimistic(Func&& func) const -> decltype(func(hot_data_));
    
    // Get write buffer (the "dark" buffer)
    OrderBookV2::ColdData* getWriteBuffer();
    
    // Get read buffer (the "light" buffer) - lock-free
    const OrderBookV2::ColdData* getReadBuffer() const;
    
    // Insert level into sorted array (maintains sort order)
    // Returns the affected position, or -1 if level was beyond MAX_DEPTH
    static int insertBidLevel(ColdData::Level* levels, uint8_t& count, Decimal price, Decimal quantity);
    static int insertAskLevel(ColdData::Level* levels, uint8_t& count, Decimal price, Decimal quantity);

    // Remove level from sorted array
    // Returns the removed position, or -1 if not found
    static int removeBidLevel(ColdData::Level* levels, uint8_t& count, Decimal price);
    static int removeAskLevel(ColdData::Level* levels, uint8_t& count, Decimal price);

    // Track whether hot data needs rebuilding after update
    bool hot_data_dirty_ = false;
};

// Template implementation for optimistic reads
template<typename Func>
auto OrderBookV2::readHotDataOptimistic(Func&& func) const -> decltype(func(hot_data_)) {
    constexpr int YIELD_THRESHOLD = 100;
    int retries = 0;
    
    while (true) {
        // Read version before accessing data
        uint64_t version_before = hot_data_.version.load(std::memory_order_acquire);
        
        // Check if version is odd (write in progress)
        if (version_before & 1) {
            ++stats_.version_retries;
            if (++retries > YIELD_THRESHOLD) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
                __asm__ volatile("pause" ::: "memory");
#elif defined(__aarch64__) || defined(__arm__)
                __asm__ volatile("yield" ::: "memory");
#endif
            }
            continue;
        }
        
        // Read the data
        auto result = func(hot_data_);
        
        // Read version after accessing data
        uint64_t version_after = hot_data_.version.load(std::memory_order_acquire);
        
        // If versions match and even, read was consistent
        if (version_before == version_after) {
            return result;
        }
        
        ++stats_.version_retries;
        if (++retries > YIELD_THRESHOLD) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
            __asm__ volatile("pause" ::: "memory");
#elif defined(__aarch64__) || defined(__arm__)
            __asm__ volatile("yield" ::: "memory");
#endif
        }
    }
}

} // namespace market_data
} // namespace chronos
