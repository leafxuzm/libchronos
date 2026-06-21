#pragma once

#include "chronos/trading/strategy.hpp"
#include "chronos/market_data/orderbook_v2.hpp"
#include "chronos/risk/risk_engine.hpp"
#include "chronos/trading/order_id_generator.hpp"
#include "chronos/trading/position_manager.hpp"
#include "chronos/utils/mpmc_queue.hpp"
#include <atomic>
#include <memory>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace chronos {
namespace trading {

/// Strategy engine — the central trading brain.
///
/// Single-threaded dispatch loop:
///   1. Pop Tick / Fill from input queues (lock-free MPMC)
///   2. Tick: update OrderBook → dispatch onTick() to subscribed strategies
///   3. Fill: update PositionManager → route onFill() by strategy_id
///
/// Strategies submit orders through StrategyContext; the engine runs pre-trade
/// risk checks, assigns order IDs, stamps strategy_id, and pushes validated
/// orders to the output queue consumed by the Order Gateway.
///
/// Performance target: onTick() dispatch <10μs (excluding strategy logic).
class StrategyEngine {
public:
    using TickQueue  = utils::MPMCQueue<Tick, 1024>;
    using OrderQueue = utils::MPMCQueue<OrderRequest, 1024>;
    using FillQueue  = utils::MPMCQueue<Fill, 1024>;

    StrategyEngine();
    ~StrategyEngine();

    StrategyEngine(const StrategyEngine&) = delete;
    StrategyEngine& operator=(const StrategyEngine&) = delete;

    // --- Lifecycle ---

    /// Start the dispatch loop in a background thread.
    void start();

    /// Signal shutdown and join the worker thread.
    void stop();

    bool isRunning() const { return running_.load(std::memory_order_acquire); }

    // --- Strategy management ---

    /// Register a strategy.  Must be called before start().
    void registerStrategy(std::unique_ptr<Strategy> strategy);

    // --- Input (market data gateway) ---

    /// Push a tick into the engine.  Thread-safe, non-blocking.
    /// Returns false if the input queue is full.
    bool pushTick(const Tick& tick) { return tick_queue_.try_push(tick); }

    /// Push a fill into the engine.  Thread-safe, non-blocking.
    /// Called by OrderGateway/ExecutionHandler when a fill arrives.
    /// Returns false if the fill queue is full.
    bool pushFill(const Fill& fill) { return fill_queue_.try_push(fill); }

    // --- Output (order gateway) ---

    /// Pop a validated order.  Called by the Order Gateway thread.
    /// Returns false if the output queue is empty.
    bool popOrder(OrderRequest& order) { return order_queue_.try_pop(order); }

    // --- Configuration ---

    RiskParameters getRiskParameters() const {
        return risk_engine_.getParameters();
    }

    void updateRiskParameters(const RiskParameters& p) {
        risk_engine_.updateParameters(p);
    }

    void setAvailableCapital(Decimal capital) {
        risk_engine_.setAvailableCapital(capital);
    }

    // --- Statistics ---

    struct Stats {
        uint64_t ticks_processed{0};
        uint64_t fills_processed{0};
        uint64_t orders_submitted{0};
        uint64_t orders_risk_rejected{0};    // rejected by RiskEngine checkOrder()
        uint64_t orders_queue_dropped{0};    // dropped because output queue is full
    };

    Stats getStats() const;

    /// Access the order output queue (for OrderGateway to consume).
    OrderQueue* getOrderQueue() { return &order_queue_; }

    /// Access the position manager for persistence / queries.
    const PositionManager& positionManager() const { return position_manager_; }
    PositionManager& positionManager() { return position_manager_; }

private:
    void run();

    // --- Owned components ---

    PositionManager                                         position_manager_;
    OrderIDGenerator                                        order_id_gen_;
    risk::RiskEngine                                        risk_engine_;

    struct StrategyEntry {
        uint32_t id;                               // assigned at registration (index+1)
        std::unique_ptr<Strategy> strategy;
        std::unordered_set<uint32_t> symbols;
        bool is_wildcard{false};
    };

    std::unordered_map<uint32_t, market_data::OrderBookV2>  order_books_;
    std::vector<StrategyEntry>                               strategies_;
    std::unordered_map<uint32_t, Strategy*>                  strategy_by_id_;

    TickQueue   tick_queue_;
    OrderQueue  order_queue_;
    FillQueue   fill_queue_;

    StrategyContext ctx_;  // reused, pointers refreshed before dispatch

    // --- Threading ---

    std::thread         worker_;
    std::atomic<bool>   running_{false};

    // --- Statistics ---

    mutable std::atomic<uint64_t> ticks_processed_{0};
    mutable std::atomic<uint64_t> fills_processed_{0};
    mutable std::atomic<uint64_t> orders_submitted_{0};
    mutable std::atomic<uint64_t> orders_risk_rejected_{0};
    mutable std::atomic<uint64_t> orders_queue_dropped_{0};
};

}  // namespace trading
}  // namespace chronos
