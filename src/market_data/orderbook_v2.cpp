#include <chronos/market_data/orderbook_v2.hpp>
#include <algorithm>
#include <chrono>
#include <cstring>

namespace chronos {
namespace market_data {

// ============================================================================
// Constructor / Destructor
// ============================================================================

OrderBookV2::OrderBookV2() 
    : write_buffer_index_(0) {
    // Initialize both cold buffers
    cold_buffers_[0].clear();
    cold_buffers_[1].clear();
    
    // Set active buffer to first one
    active_cold_buffer_.store(&cold_buffers_[0], std::memory_order_release);
}

OrderBookV2::~OrderBookV2() = default;

// ============================================================================
// Public Methods
// ============================================================================

void OrderBookV2::update(const Tick& tick) {
    updateLevels(tick);
    rebuildHotData();
    swapColdBuffers();
    stats_.total_updates++;
}

std::optional<Decimal> OrderBookV2::getBestBid() const {
    return readHotDataOptimistic([](const HotData& data) -> std::optional<Decimal> {
        if (data.bid_count > 0) {
            return data.bid_prices[0];
        }
        return std::nullopt;
    });
}

std::optional<Decimal> OrderBookV2::getBestAsk() const {
    return readHotDataOptimistic([](const HotData& data) -> std::optional<Decimal> {
        if (data.ask_count > 0) {
            return data.ask_prices[0];
        }
        return std::nullopt;
    });
}

std::optional<Decimal> OrderBookV2::getMidPrice() const {
    return readHotDataOptimistic([](const HotData& data) -> std::optional<Decimal> {
        if (data.bid_count > 0 && data.ask_count > 0) {
            return (data.bid_prices[0] + data.ask_prices[0]) / Decimal(2);
        }
        return std::nullopt;
    });
}

std::optional<Decimal> OrderBookV2::getSpread() const {
    return readHotDataOptimistic([](const HotData& data) -> std::optional<Decimal> {
        if (data.bid_count > 0 && data.ask_count > 0) {
            return data.ask_prices[0] - data.bid_prices[0];
        }
        return std::nullopt;
    });
}

std::array<PriceLevel, 5> OrderBookV2::getTop5Bids() const {
    return readHotDataOptimistic([](const HotData& data) -> std::array<PriceLevel, 5> {
        std::array<PriceLevel, 5> levels;
        for (size_t i = 0; i < HOT_DEPTH; ++i) {
            if (i < data.bid_count) {
                levels[i] = PriceLevel(data.bid_prices[i], data.bid_quantities[i]);
            } else {
                levels[i] = PriceLevel();
            }
        }
        return levels;
    });
}

std::array<PriceLevel, 5> OrderBookV2::getTop5Asks() const {
    return readHotDataOptimistic([](const HotData& data) -> std::array<PriceLevel, 5> {
        std::array<PriceLevel, 5> levels;
        for (size_t i = 0; i < HOT_DEPTH; ++i) {
            if (i < data.ask_count) {
                levels[i] = PriceLevel(data.ask_prices[i], data.ask_quantities[i]);
            } else {
                levels[i] = PriceLevel();
            }
        }
        return levels;
    });
}

std::optional<PriceLevel> OrderBookV2::getBidLevel(size_t level) const {
    if (level >= MAX_DEPTH) {
        return std::nullopt;
    }
    
    // Lock-free read via atomic pointer
    const ColdData* buffer = getReadBuffer();
    
    if (level < buffer->bid_count) {
        stats_.cold_path_reads++;
        return PriceLevel(buffer->bids[level].price, buffer->bids[level].quantity);
    }
    
    return std::nullopt;
}

std::optional<PriceLevel> OrderBookV2::getAskLevel(size_t level) const {
    if (level >= MAX_DEPTH) {
        return std::nullopt;
    }
    
    // Lock-free read via atomic pointer
    const ColdData* buffer = getReadBuffer();
    
    if (level < buffer->ask_count) {
        stats_.cold_path_reads++;
        return PriceLevel(buffer->asks[level].price, buffer->asks[level].quantity);
    }
    
    return std::nullopt;
}

std::vector<PriceLevel> OrderBookV2::getBidLevels(size_t max_levels) const {
    // Lock-free read via atomic pointer
    const ColdData* buffer = getReadBuffer();
    
    size_t count = std::min(max_levels, static_cast<size_t>(buffer->bid_count));
    std::vector<PriceLevel> levels;
    levels.reserve(count);
    
    for (size_t i = 0; i < count; ++i) {
        if (buffer->bids[i].is_valid()) {
            levels.emplace_back(buffer->bids[i].price, buffer->bids[i].quantity);
        }
    }
    
    stats_.cold_path_reads++;
    return levels;
}

std::vector<PriceLevel> OrderBookV2::getAskLevels(size_t max_levels) const {
    // Lock-free read via atomic pointer
    const ColdData* buffer = getReadBuffer();
    
    size_t count = std::min(max_levels, static_cast<size_t>(buffer->ask_count));
    std::vector<PriceLevel> levels;
    levels.reserve(count);
    
    for (size_t i = 0; i < count; ++i) {
        if (buffer->asks[i].is_valid()) {
            levels.emplace_back(buffer->asks[i].price, buffer->asks[i].quantity);
        }
    }
    
    stats_.cold_path_reads++;
    return levels;
}

std::pair<std::array<PriceLevel, 20>, uint8_t> OrderBookV2::getBidLevelsFast() const {
    std::pair<std::array<PriceLevel, 20>, uint8_t> result;
    const ColdData* buffer = getReadBuffer();

    result.second = buffer->bid_count;
    for (size_t i = 0; i < buffer->bid_count; ++i) {
        result.first[i] = PriceLevel(buffer->bids[i].price, buffer->bids[i].quantity);
    }

    stats_.cold_path_reads++;
    return result;
}

std::pair<std::array<PriceLevel, 20>, uint8_t> OrderBookV2::getAskLevelsFast() const {
    std::pair<std::array<PriceLevel, 20>, uint8_t> result;
    const ColdData* buffer = getReadBuffer();

    result.second = buffer->ask_count;
    for (size_t i = 0; i < buffer->ask_count; ++i) {
        result.first[i] = PriceLevel(buffer->asks[i].price, buffer->asks[i].quantity);
    }

    stats_.cold_path_reads++;
    return result;
}

OrderBookSnapshot OrderBookV2::generateSnapshot() const {
    const ColdData* buffer = getReadBuffer();
    
    OrderBookSnapshot snapshot;
    snapshot.timestamp_us = buffer->last_update_time;
    
    // Copy all bid levels
    for (size_t i = 0; i < buffer->bid_count; ++i) {
        if (buffer->bids[i].is_valid()) {
            snapshot.bids.emplace_back(buffer->bids[i].price, buffer->bids[i].quantity);
        }
    }
    
    // Copy all ask levels
    for (size_t i = 0; i < buffer->ask_count; ++i) {
        if (buffer->asks[i].is_valid()) {
            snapshot.asks.emplace_back(buffer->asks[i].price, buffer->asks[i].quantity);
        }
    }
    
    return snapshot;
}

void OrderBookV2::clear() {
    // Clear write buffer
    ColdData* write_buf = getWriteBuffer();
    write_buf->clear();
    
    // Clear hot data
    hot_data_.version.fetch_add(1, std::memory_order_release);  // Start write
    hot_data_.bid_count = 0;
    hot_data_.ask_count = 0;
    std::memset(hot_data_.bid_prices, 0, sizeof(hot_data_.bid_prices));
    std::memset(hot_data_.bid_quantities, 0, sizeof(hot_data_.bid_quantities));
    std::memset(hot_data_.ask_prices, 0, sizeof(hot_data_.ask_prices));
    std::memset(hot_data_.ask_quantities, 0, sizeof(hot_data_.ask_quantities));
    hot_data_.version.fetch_add(1, std::memory_order_release);  // End write
    
    // Swap buffers
    swapColdBuffers();
}

bool OrderBookV2::empty() const {
    const ColdData* buffer = getReadBuffer();
    return buffer->bid_count == 0 && buffer->ask_count == 0;
}

uint64_t OrderBookV2::getLastUpdateTime() const {
    return hot_data_.last_update_time;
}

OrderBookV2::Statistics OrderBookV2::getStatistics() const {
    return stats_;
}

// ============================================================================
// Private Helper Methods
// ============================================================================

void OrderBookV2::updateLevels(const Tick& tick) {
    ColdData* write_buf = getWriteBuffer();

    bool is_bid = (tick.side == TickSide::BID);
    int affected_pos = -1;

    if (is_bid) {
        if (tick.quantity > Decimal(0)) {
            affected_pos = insertBidLevel(write_buf->bids, write_buf->bid_count, tick.price, tick.quantity);
        } else {
            affected_pos = removeBidLevel(write_buf->bids, write_buf->bid_count, tick.price);
        }
    } else {
        if (tick.quantity > Decimal(0)) {
            affected_pos = insertAskLevel(write_buf->asks, write_buf->ask_count, tick.price, tick.quantity);
        } else {
            affected_pos = removeAskLevel(write_buf->asks, write_buf->ask_count, tick.price);
        }
    }

    // Track whether top 5 was affected for rebuildHotData fast-path
    hot_data_dirty_ = (affected_pos >= 0 && affected_pos < static_cast<int>(HOT_DEPTH));

    // Update timestamp (use tick's receive timestamp instead of expensive system_clock::now)
    write_buf->last_update_time = tick.receive_timestamp_us;
}

void OrderBookV2::rebuildHotData() {
    if (!hot_data_dirty_) {
        return;
    }
    hot_data_dirty_ = false;

    ColdData* write_buf = getWriteBuffer();

    // Increment version (odd = write in progress)
    hot_data_.version.fetch_add(1, std::memory_order_release);

    // Copy top 5 bids
    hot_data_.bid_count = std::min(static_cast<uint8_t>(HOT_DEPTH), write_buf->bid_count);
    for (size_t i = 0; i < hot_data_.bid_count; ++i) {
        hot_data_.bid_prices[i] = write_buf->bids[i].price;
        hot_data_.bid_quantities[i] = write_buf->bids[i].quantity;
    }

    // Copy top 5 asks
    hot_data_.ask_count = std::min(static_cast<uint8_t>(HOT_DEPTH), write_buf->ask_count);
    for (size_t i = 0; i < hot_data_.ask_count; ++i) {
        hot_data_.ask_prices[i] = write_buf->asks[i].price;
        hot_data_.ask_quantities[i] = write_buf->asks[i].quantity;
    }

    // Update timestamp
    hot_data_.last_update_time = write_buf->last_update_time;

    // Increment version (even = write complete)
    hot_data_.version.fetch_add(1, std::memory_order_release);
}

void OrderBookV2::swapColdBuffers() {
    write_buffer_index_ = 1 - write_buffer_index_;

    ColdData* new_active = &cold_buffers_[1 - write_buffer_index_];
    active_cold_buffer_.store(new_active, std::memory_order_release);

    // Explicit memcpy: ColdData is trivially copyable (640 bytes)
    ColdData* write_buf = getWriteBuffer();
    std::memcpy(write_buf, new_active, sizeof(ColdData));

    // Stats are deliberately unprotected to avoid locking overhead on the hot path
    stats_.buffer_swaps++;
}

OrderBookV2::ColdData* OrderBookV2::getWriteBuffer() {
    return &cold_buffers_[write_buffer_index_];
}

const OrderBookV2::ColdData* OrderBookV2::getReadBuffer() const {
    return active_cold_buffer_.load(std::memory_order_acquire);
}

// ============================================================================
// Static Helper Methods for Sorted Array Operations
// ============================================================================

int OrderBookV2::insertBidLevel(ColdData::Level* levels, uint8_t& count, Decimal price, Decimal quantity) {
    // Bids: descending order (highest first)
    // Binary search for insertion point: first level with price <= price
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (levels[mid].price > price) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    // Check exact match (update existing)
    if (lo < count && levels[lo].price == price) {
        levels[lo].quantity = quantity;
        return static_cast<int>(lo);
    }

    size_t insert_pos = lo;

    // Trim if at capacity
    if (count >= MAX_DEPTH) {
        if (insert_pos >= MAX_DEPTH) {
            return -1;
        }
        count = MAX_DEPTH - 1;
    }

    // Shift and insert
    for (size_t i = count; i > insert_pos; --i) {
        levels[i] = levels[i - 1];
    }
    levels[insert_pos] = ColdData::Level(price, quantity);
    count++;
    return static_cast<int>(insert_pos);
}

int OrderBookV2::insertAskLevel(ColdData::Level* levels, uint8_t& count, Decimal price, Decimal quantity) {
    // Asks: ascending order (lowest first)
    // Binary search for insertion point: first level with price >= price
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (levels[mid].price < price) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    // Check exact match (update existing)
    if (lo < count && levels[lo].price == price) {
        levels[lo].quantity = quantity;
        return static_cast<int>(lo);
    }

    size_t insert_pos = lo;

    // Trim if at capacity
    if (count >= MAX_DEPTH) {
        if (insert_pos >= MAX_DEPTH) {
            return -1;
        }
        count = MAX_DEPTH - 1;
    }

    // Shift and insert
    for (size_t i = count; i > insert_pos; --i) {
        levels[i] = levels[i - 1];
    }
    levels[insert_pos] = ColdData::Level(price, quantity);
    count++;
    return static_cast<int>(insert_pos);
}

int OrderBookV2::removeBidLevel(ColdData::Level* levels, uint8_t& count, Decimal price) {
    // Binary search for the level to remove
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (levels[mid].price > price) {
            lo = mid + 1;
        } else if (levels[mid].price < price) {
            hi = mid;
        } else {
            // Found — shift and remove
            for (size_t j = mid; j < count - 1; ++j) {
                levels[j] = levels[j + 1];
            }
            count--;
            return static_cast<int>(mid);
        }
    }
    return -1;
}

int OrderBookV2::removeAskLevel(ColdData::Level* levels, uint8_t& count, Decimal price) {
    // Binary search for the level to remove
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (levels[mid].price < price) {
            lo = mid + 1;
        } else if (levels[mid].price > price) {
            hi = mid;
        } else {
            // Found — shift and remove
            for (size_t j = mid; j < count - 1; ++j) {
                levels[j] = levels[j + 1];
            }
            count--;
            return static_cast<int>(mid);
        }
    }
    return -1;
}

} // namespace market_data
} // namespace chronos
