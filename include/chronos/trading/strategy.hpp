#pragma once

#include "chronos/core/types.hpp"
#include "chronos/market_data/orderbook_v2.hpp"
#include "chronos/risk/risk_engine.hpp"
#include "chronos/trading/order_id_generator.hpp"
#include "chronos/trading/position_manager.hpp"
#include "chronos/utils/mpmc_queue.hpp"
#include <array>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace chronos {
namespace backtest { class BacktestEngine; }
namespace trading {

class StrategyEngine;

/// Strategy context — controlled access to market data, positions, and order
/// submission.  Created and configured by StrategyEngine before each callback.
class StrategyContext {
public:
    /// Get best bid for a symbol, or nullopt if no bids.
    std::optional<Decimal> getBestBid(uint32_t symbol_id) const;

    /// Get best ask for a symbol, or nullopt if no asks.
    std::optional<Decimal> getBestAsk(uint32_t symbol_id) const;

    /// Get mid price, or nullopt if missing bid or ask.
    std::optional<Decimal> getMidPrice(uint32_t symbol_id) const;

    /// Get top 5 bid levels (lock-free hot path).
    std::array<PriceLevel, 5> getTop5Bids(uint32_t symbol_id) const;

    /// Get top 5 ask levels (lock-free hot path).
    std::array<PriceLevel, 5> getTop5Asks(uint32_t symbol_id) const;

    /// Get position for a symbol, or nullptr if none.
    const Position* getPosition(uint32_t symbol_id) const;

    /// Available capital for risk checks.
    Decimal getAvailableCapital() const;

    /// Submit an order through the risk engine.
    /// Returns the assigned order ID, or 0 if rejected.
    uint64_t submitOrder(const OrderRequest& order);

    /// Request cancellation of an existing order.
    /// Returns true if the cancel request was queued.
    bool cancelOrder(uint64_t order_id);

    /// Current timestamp (set by engine before each callback).
    uint64_t currentTimestampUs() const { return timestamp_us_; }

private:
    friend class StrategyEngine;
    friend class backtest::BacktestEngine;

    std::unordered_map<uint32_t, market_data::OrderBookV2>* order_books_{nullptr};
    risk::RiskEngine*                     risk_engine_{nullptr};
    PositionManager*                      position_manager_{nullptr};
    OrderIDGenerator*                     order_id_gen_{nullptr};
    utils::MPMCQueue<OrderRequest, 1024>* order_output_{nullptr};
    std::atomic<uint64_t>*                orders_submitted_{nullptr};
    std::atomic<uint64_t>*                orders_risk_rejected_{nullptr};
    std::atomic<uint64_t>*                orders_queue_dropped_{nullptr};
    uint64_t                              timestamp_us_{0};
    uint32_t                              current_strategy_id_{0};
};

/// Abstract strategy — implement onTick() to define trading logic.
///
/// Lifecycle: onLoad() → onTick()/onFill()/onTimer() → onUnload()
class Strategy {
public:
    virtual ~Strategy() = default;

    /// Unique strategy name (used for logging / stats).
    virtual const char* name() const = 0;

    /// Symbols this strategy subscribes to.
    virtual std::vector<uint32_t> symbols() const { return {}; }

    /// Called once when the strategy is loaded.
    virtual void onLoad(StrategyContext& /*ctx*/) {}

    /// Called once when the strategy is unloaded.
    virtual void onUnload(StrategyContext& /*ctx*/) {}

    /// Called for every tick on a subscribed symbol.
    virtual void onTick(const Tick& tick, StrategyContext& ctx) = 0;

    /// Called when one of this strategy's orders is filled.
    virtual void onFill(const Fill& fill, StrategyContext& /*ctx*/) {}

    /// Called periodically (~100ms) for housekeeping / signal generation.
    virtual void onTimer(uint64_t timestamp_us, StrategyContext& /*ctx*/) {}
};

}  // namespace trading
}  // namespace chronos
