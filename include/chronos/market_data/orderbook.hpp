#pragma once

#include <chronos/core/types.hpp>
#include <map>
#include <shared_mutex>
#include <optional>
#include <vector>
#include <chrono>
#include <atomic>

namespace chronos {
namespace market_data {

/**
 * @brief High-performance thread-safe OrderBook implementation
 * 
 * Uses hot/cold data separation for optimal cache performance:
 * - Hot data: frequently accessed best prices (cache-line aligned)
 * - Cold data: full depth levels (less frequently accessed)
 * 
 * Supports up to 20 levels of depth on each side.
 */
class OrderBook {
public:
    OrderBook() = default;
    ~OrderBook() = default;

    // Non-copyable, non-movable
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) = delete;
    OrderBook& operator=(OrderBook&&) = delete;

    /**
     * @brief Update orderbook with tick data
     * 
     * @param tick Tick containing price level update
     */
    void update(const Tick& tick);

    /**
     * @brief Get best bid price (lock-free hot path)
     * 
     * @return Best bid price, or nullopt if no bids
     */
    std::optional<Decimal> getBestBid() const;

    /**
     * @brief Get best ask price (lock-free hot path)
     * 
     * @return Best ask price, or nullopt if no asks
     */
    std::optional<Decimal> getBestAsk() const;

    /**
     * @brief Get mid price (average of best bid and ask)
     * 
     * @return Mid price, or nullopt if no bid or ask
     */
    std::optional<Decimal> getMidPrice() const;

    /**
     * @brief Get spread (difference between best ask and bid)
     * 
     * @return Spread, or nullopt if no bid or ask
     */
    std::optional<Decimal> getSpread() const;

    /**
     * @brief Get best bid and ask atomically
     * 
     * @return Pair of (bid, ask), either may be nullopt
     */
    std::pair<std::optional<Decimal>, std::optional<Decimal>> getBestBidAsk() const;

    /**
     * @brief Get bid level at specified depth
     * 
     * @param level Level (0 = best, 1 = second best, etc.)
     * @return Price level, or nullopt if level doesn't exist
     */
    std::optional<PriceLevel> getBidLevel(size_t level) const;

    /**
     * @brief Get ask level at specified depth
     * 
     * @param level Level (0 = best, 1 = second best, etc.)
     * @return Price level, or nullopt if level doesn't exist
     */
    std::optional<PriceLevel> getAskLevel(size_t level) const;

    /**
     * @brief Get all bid levels up to max depth
     * 
     * @param max_levels Maximum number of levels to return (default: 20)
     * @return Vector of bid levels, sorted by price (highest first)
     */
    std::vector<PriceLevel> getBidLevels(size_t max_levels = 20) const;

    /**
     * @brief Get all ask levels up to max depth
     * 
     * @param max_levels Maximum number of levels to return (default: 20)
     * @return Vector of ask levels, sorted by price (lowest first)
     */
    std::vector<PriceLevel> getAskLevels(size_t max_levels = 20) const;

    /**
     * @brief Generate orderbook snapshot
     * 
     * @return Complete orderbook snapshot
     */
    OrderBookSnapshot generateSnapshot() const;

    /**
     * @brief Rebuild orderbook from snapshot
     * 
     * @param snapshot Orderbook snapshot to restore from
     */
    void rebuildFromSnapshot(const OrderBookSnapshot& snapshot);

    /**
     * @brief Clear all price levels
     */
    void clear();

    /**
     * @brief Check if orderbook is empty
     * 
     * @return true if no bid or ask levels exist
     */
    bool empty() const;

    /**
     * @brief Get number of bid levels
     */
    size_t getBidDepth() const;

    /**
     * @brief Get number of ask levels
     */
    size_t getAskDepth() const;

    /**
     * @brief Get last update timestamp
     */
    uint64_t getLastUpdateTime() const;

    /**
     * @brief Statistics for monitoring
     */
    struct Statistics {
        uint64_t total_updates = 0;
        uint64_t bid_updates = 0;
        uint64_t ask_updates = 0;
        uint64_t level_additions = 0;
        uint64_t level_removals = 0;
        uint64_t hot_path_reads = 0;
        uint64_t cold_path_reads = 0;
        std::chrono::microseconds avg_update_latency{0};
    };

    Statistics getStatistics() const;

private:
    // Hot data: frequently accessed best prices (cache-line aligned)
    struct alignas(64) HotData {
        Decimal best_bid_price = Decimal(0);
        Decimal best_bid_quantity = Decimal(0);
        Decimal best_ask_price = Decimal(0);
        Decimal best_ask_quantity = Decimal(0);
        uint64_t last_update_time = 0;
        std::atomic<uint64_t> version{0};  // For optimistic reads
        bool has_bids = false;
        bool has_asks = false;
    };
    
    // Cold data: full depth (less frequently accessed)
    struct ColdData {
        // Price levels: price -> quantity
        // Using std::map for automatic sorting
        // Bids: highest price first (reverse order)
        // Asks: lowest price first (normal order)
        std::map<Decimal, Decimal, std::greater<Decimal>> bids_;  // Descending order
        std::map<Decimal, Decimal> asks_;                         // Ascending order
        
        mutable Statistics statistics_;
    };

    // Separate hot and cold data for better cache performance
    mutable HotData hot_data_;
    ColdData cold_data_;

    // Reader-writer mutex for cold data access
    mutable std::shared_mutex cold_mutex_;
    
    // Spinlock for hot data (very short critical sections)
    mutable std::atomic_flag hot_lock_ = ATOMIC_FLAG_INIT;

    // Maximum depth to maintain
    static constexpr size_t MAX_DEPTH = 20;

    // Helper methods
    void updateBidLevel(Decimal price, Decimal quantity);
    void updateAskLevel(Decimal price, Decimal quantity);
    void updateHotData(uint64_t receive_timestamp_us);
    void trimDepth();
    
    // Lock-free hot data access helpers
    void lockHotData() const;
    void unlockHotData() const;
    
    // Optimistic read for hot data
    template<typename Func>
    auto readHotDataOptimistic(Func&& func) const -> decltype(func(hot_data_));
};

} // namespace market_data
} // namespace chronos