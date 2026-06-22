#include <chronos/market_data/orderbook.hpp>
#include <algorithm>
#include <chrono>
#include <mutex>

namespace chronos {
namespace market_data {

// ============================================================================
// Public Methods
// ============================================================================

void OrderBook::update(const Tick& tick) {
    auto start = std::chrono::high_resolution_clock::now();
    
    // Determine if this is a bid or ask update
    bool is_bid = (tick.side == TickSide::BID);
    
    if (is_bid) {
        updateBidLevel(tick.price, tick.quantity);
    } else {
        updateAskLevel(tick.price, tick.quantity);
    }
    
    // Update hot data
    updateHotData(tick.receive_timestamp_us);
    
    // Trim depth if necessary
    trimDepth();
    
    // Update statistics
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    std::unique_lock<std::shared_mutex> lock(cold_mutex_);
    cold_data_.statistics_.total_updates++;
    if (is_bid) {
        cold_data_.statistics_.bid_updates++;
    } else {
        cold_data_.statistics_.ask_updates++;
    }
    cold_data_.statistics_.avg_update_latency = duration;
}

std::optional<Decimal> OrderBook::getBestBid() const {
    return readHotDataOptimistic([](const HotData& data) -> std::optional<Decimal> {
        if (data.has_bids) {
            return data.best_bid_price;
        }
        return std::nullopt;
    });
}

std::optional<Decimal> OrderBook::getBestAsk() const {
    return readHotDataOptimistic([](const HotData& data) -> std::optional<Decimal> {
        if (data.has_asks) {
            return data.best_ask_price;
        }
        return std::nullopt;
    });
}

std::optional<Decimal> OrderBook::getMidPrice() const {
    return readHotDataOptimistic([](const HotData& data) -> std::optional<Decimal> {
        if (data.has_bids && data.has_asks) {
            return (data.best_bid_price + data.best_ask_price) / Decimal(2);
        }
        return std::nullopt;
    });
}

std::optional<Decimal> OrderBook::getSpread() const {
    return readHotDataOptimistic([](const HotData& data) -> std::optional<Decimal> {
        if (data.has_bids && data.has_asks) {
            return data.best_ask_price - data.best_bid_price;
        }
        return std::nullopt;
    });
}

std::pair<std::optional<Decimal>, std::optional<Decimal>> OrderBook::getBestBidAsk() const {
    return readHotDataOptimistic([](const HotData& data) 
        -> std::pair<std::optional<Decimal>, std::optional<Decimal>> {
        std::optional<Decimal> bid = data.has_bids ? 
            std::optional<Decimal>(data.best_bid_price) : std::nullopt;
        std::optional<Decimal> ask = data.has_asks ? 
            std::optional<Decimal>(data.best_ask_price) : std::nullopt;
        return {bid, ask};
    });
}

std::optional<PriceLevel> OrderBook::getBidLevel(size_t level) const {
    std::shared_lock<std::shared_mutex> lock(cold_mutex_);
    
    if (level >= cold_data_.bids_.size()) {
        return std::nullopt;
    }
    
    auto it = cold_data_.bids_.begin();
    std::advance(it, level);
    
    cold_data_.statistics_.cold_path_reads++;
    return PriceLevel{it->first, it->second};
}

std::optional<PriceLevel> OrderBook::getAskLevel(size_t level) const {
    std::shared_lock<std::shared_mutex> lock(cold_mutex_);
    
    if (level >= cold_data_.asks_.size()) {
        return std::nullopt;
    }
    
    auto it = cold_data_.asks_.begin();
    std::advance(it, level);
    
    cold_data_.statistics_.cold_path_reads++;
    return PriceLevel{it->first, it->second};
}

std::vector<PriceLevel> OrderBook::getBidLevels(size_t max_levels) const {
    std::shared_lock<std::shared_mutex> lock(cold_mutex_);
    
    std::vector<PriceLevel> levels;
    levels.reserve(std::min(max_levels, cold_data_.bids_.size()));
    
    size_t count = 0;
    for (const auto& [price, quantity] : cold_data_.bids_) {
        if (count >= max_levels) break;
        levels.emplace_back(price, quantity);
        count++;
    }
    
    cold_data_.statistics_.cold_path_reads++;
    return levels;
}

std::vector<PriceLevel> OrderBook::getAskLevels(size_t max_levels) const {
    std::shared_lock<std::shared_mutex> lock(cold_mutex_);
    
    std::vector<PriceLevel> levels;
    levels.reserve(std::min(max_levels, cold_data_.asks_.size()));
    
    size_t count = 0;
    for (const auto& [price, quantity] : cold_data_.asks_) {
        if (count >= max_levels) break;
        levels.emplace_back(price, quantity);
        count++;
    }
    
    cold_data_.statistics_.cold_path_reads++;
    return levels;
}

OrderBookSnapshot OrderBook::generateSnapshot() const {
    std::shared_lock<std::shared_mutex> lock(cold_mutex_);
    
    OrderBookSnapshot snapshot;
    snapshot.timestamp_us = hot_data_.last_update_time;
    
    // Copy all bid levels
    for (const auto& [price, quantity] : cold_data_.bids_) {
        snapshot.bids.emplace_back(price, quantity);
    }
    
    // Copy all ask levels
    for (const auto& [price, quantity] : cold_data_.asks_) {
        snapshot.asks.emplace_back(price, quantity);
    }
    
    return snapshot;
}

void OrderBook::rebuildFromSnapshot(const OrderBookSnapshot& snapshot) {
    std::unique_lock<std::shared_mutex> lock(cold_mutex_);
    
    // Clear existing data
    cold_data_.bids_.clear();
    cold_data_.asks_.clear();
    
    // Rebuild from snapshot
    for (const auto& level : snapshot.bids) {
        cold_data_.bids_[level.price] = level.quantity;
    }
    
    for (const auto& level : snapshot.asks) {
        cold_data_.asks_[level.price] = level.quantity;
    }
    
    // Update hot data (use system clock for infrequent snapshot rebuild)
    lock.unlock();
    auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    updateHotData(now_us);
}

void OrderBook::clear() {
    std::unique_lock<std::shared_mutex> lock(cold_mutex_);
    
    cold_data_.bids_.clear();
    cold_data_.asks_.clear();
    
    // Clear hot data
    lockHotData();
    hot_data_.version.fetch_add(1, std::memory_order_release);  // Start write
    hot_data_.has_bids = false;
    hot_data_.has_asks = false;
    hot_data_.best_bid_price = Decimal(0);
    hot_data_.best_bid_quantity = Decimal(0);
    hot_data_.best_ask_price = Decimal(0);
    hot_data_.best_ask_quantity = Decimal(0);
    hot_data_.version.fetch_add(1, std::memory_order_release);  // End write
    unlockHotData();
}

bool OrderBook::empty() const {
    std::shared_lock<std::shared_mutex> lock(cold_mutex_);
    return cold_data_.bids_.empty() && cold_data_.asks_.empty();
}

size_t OrderBook::getBidDepth() const {
    std::shared_lock<std::shared_mutex> lock(cold_mutex_);
    return cold_data_.bids_.size();
}

size_t OrderBook::getAskDepth() const {
    std::shared_lock<std::shared_mutex> lock(cold_mutex_);
    return cold_data_.asks_.size();
}

uint64_t OrderBook::getLastUpdateTime() const {
    return hot_data_.last_update_time;
}

OrderBook::Statistics OrderBook::getStatistics() const {
    std::shared_lock<std::shared_mutex> lock(cold_mutex_);
    return cold_data_.statistics_;
}

// ============================================================================
// Private Helper Methods
// ============================================================================

void OrderBook::updateBidLevel(Decimal price, Decimal quantity) {
    std::unique_lock<std::shared_mutex> lock(cold_mutex_);
    
    if (quantity > Decimal(0)) {
        // Add or update level
        auto it = cold_data_.bids_.find(price);
        if (it == cold_data_.bids_.end()) {
            cold_data_.statistics_.level_additions++;
        }
        cold_data_.bids_[price] = quantity;
    } else {
        // Remove level
        auto it = cold_data_.bids_.find(price);
        if (it != cold_data_.bids_.end()) {
            cold_data_.bids_.erase(it);
            cold_data_.statistics_.level_removals++;
        }
    }
}

void OrderBook::updateAskLevel(Decimal price, Decimal quantity) {
    std::unique_lock<std::shared_mutex> lock(cold_mutex_);
    
    if (quantity > Decimal(0)) {
        // Add or update level
        auto it = cold_data_.asks_.find(price);
        if (it == cold_data_.asks_.end()) {
            cold_data_.statistics_.level_additions++;
        }
        cold_data_.asks_[price] = quantity;
    } else {
        // Remove level
        auto it = cold_data_.asks_.find(price);
        if (it != cold_data_.asks_.end()) {
            cold_data_.asks_.erase(it);
            cold_data_.statistics_.level_removals++;
        }
    }
}

void OrderBook::updateHotData(uint64_t receive_timestamp_us) {
    std::shared_lock<std::shared_mutex> lock(cold_mutex_);

    // Lock hot data for writing
    lockHotData();

    // Increment version (odd = write in progress)
    hot_data_.version.fetch_add(1, std::memory_order_release);

    // Update best bid
    if (!cold_data_.bids_.empty()) {
        auto best_bid = cold_data_.bids_.begin();
        hot_data_.best_bid_price = best_bid->first;
        hot_data_.best_bid_quantity = best_bid->second;
        hot_data_.has_bids = true;
    } else {
        hot_data_.has_bids = false;
    }

    // Update best ask
    if (!cold_data_.asks_.empty()) {
        auto best_ask = cold_data_.asks_.begin();
        hot_data_.best_ask_price = best_ask->first;
        hot_data_.best_ask_quantity = best_ask->second;
        hot_data_.has_asks = true;
    } else {
        hot_data_.has_asks = false;
    }

    // Update timestamp (use tick's receive timestamp instead of expensive system_clock::now)
    hot_data_.last_update_time = receive_timestamp_us;

    // Increment version (even = write complete)
    hot_data_.version.fetch_add(1, std::memory_order_release);

    unlockHotData();
}

void OrderBook::trimDepth() {
    std::unique_lock<std::shared_mutex> lock(cold_mutex_);
    
    // Trim bids if exceeds max depth
    while (cold_data_.bids_.size() > MAX_DEPTH) {
        auto it = cold_data_.bids_.end();
        --it;  // Last element (worst price)
        cold_data_.bids_.erase(it);
    }
    
    // Trim asks if exceeds max depth
    while (cold_data_.asks_.size() > MAX_DEPTH) {
        auto it = cold_data_.asks_.end();
        --it;  // Last element (worst price)
        cold_data_.asks_.erase(it);
    }
}

void OrderBook::lockHotData() const {
    while (hot_lock_.test_and_set(std::memory_order_acquire)) {
        // Spin
    }
}

void OrderBook::unlockHotData() const {
    hot_lock_.clear(std::memory_order_release);
}

template<typename Func>
auto OrderBook::readHotDataOptimistic(Func&& func) const -> decltype(func(hot_data_)) {
    constexpr int MAX_RETRIES = 100;
    
    for (int retry = 0; retry < MAX_RETRIES; ++retry) {
        // Read version before
        uint64_t version_before = hot_data_.version.load(std::memory_order_acquire);
        
        // Check if write in progress (odd version)
        if (version_before & 1) {
            continue;
        }
        
        // Read the data
        auto result = func(hot_data_);
        
        // Read version after
        uint64_t version_after = hot_data_.version.load(std::memory_order_acquire);
        
        // If versions match, read was consistent
        if (version_before == version_after) {
            // Statistics deliberately unprotected to avoid locking on hot path
            cold_data_.statistics_.hot_path_reads++;
            return result;
        }
    }
    
    // Fallback: should rarely happen
    return func(hot_data_);
}

} // namespace market_data
} // namespace chronos
