#pragma once

#include "chronos/core/types.hpp"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace chronos {
namespace trading {

/// Thread-safe position manager with P&L tracking.
///
/// All mutation goes through updatePosition(const Fill&) which recalculates
/// average price via weighted average and computes realized P&L on reductions.
///
/// Provides a cached total notional value (lock-free atomic read) for the
/// RiskEngine hot path, avoiding O(N) position scans on every risk check.
class PositionManager {
public:
    PositionManager() = default;

    PositionManager(const PositionManager&) = delete;
    PositionManager& operator=(const PositionManager&) = delete;

    // --- Mutations ---

    /// Update a position from a fill event.  Returns a pointer to the updated
    /// Position (or nullptr if the fill quantity is zero).
    const Position* updatePosition(const Fill& fill);

    // --- Queries ---

    /// Get the position for a symbol, or nullptr if none.
    /// Pointer is valid only until the next mutation (updatePosition, fromJson, clear).
    const Position* getPosition(uint32_t symbol_id) const;

    /// Snapshot of all current positions (by-value copy under lock).
    std::vector<Position> getAllPositions() const;

    /// Number of positions with non-zero quantity.
    size_t size() const;

    /// Total unrealized P&L across all positions given current market prices.
    Decimal getUnrealizedPnL(
        const std::unordered_map<uint32_t, Decimal>& current_prices) const;

    /// Total notional value of all positions (sum of |qty * price|).
    /// Iterates positions — O(N).  Prefer getTotalValueCached() in hot paths.
    Decimal getTotalValue(
        const std::unordered_map<uint32_t, Decimal>& current_prices) const;

    /// Lock-free cached total notional value for the RiskEngine hot path.
    /// Updated atomically inside updatePosition() under the same mutex.
    Decimal getTotalValueCached() const {
        return Decimal::from_raw_value(
            total_value_raw_.load(std::memory_order_acquire));
    }

    // --- Lifecycle ---

    /// Remove all positions and reset total value cache.
    void clear();

    // --- Persistence ---

    bool savePositions(const std::string& filepath) const;
    bool loadPositions(const std::string& filepath);

    /// Low-level JSON serialization (for ZMQ / programmatic use).
    nlohmann::json toJson() const;
    bool fromJson(const nlohmann::json& j);

private:
    mutable std::mutex mutex_;
    std::unordered_map<uint32_t, Position> positions_;

    std::atomic<int64_t> total_value_raw_{0};

    void recalcTotalValue();

    static Decimal calcWeightedAverage(
        Decimal old_qty, Decimal old_avg,
        Decimal fill_qty, Decimal fill_price);

    static Decimal calcRealizedPnLOnReduce(
        const Position& pos, Decimal fill_price, Decimal reduced_abs);
};

}  // namespace trading
}  // namespace chronos
