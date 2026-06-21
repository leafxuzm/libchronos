#pragma once

#include "chronos/backtest/metrics.hpp"
#include "chronos/backtest/time_replayer.hpp"
#include "chronos/core/types.hpp"
#include "chronos/logging/log_reader.hpp"
#include "chronos/market_data/orderbook_v2.hpp"
#include "chronos/risk/risk_engine.hpp"
#include "chronos/trading/order_id_generator.hpp"
#include "chronos/trading/position_manager.hpp"
#include "chronos/trading/strategy.hpp"
#include "chronos/utils/mpmc_queue.hpp"
#include <memory>
#include <vector>

namespace chronos {
namespace backtest {

// ============================================================================
// BacktestConfig
// ============================================================================

struct BacktestConfig {
    enum FillMode {
        IMMEDIATE,    // fill at current best bid/ask immediately
        NEXT_TICK,    // fill at next tick's price
        CONSERVATIVE  // fill at worst price since order placed
    };

    FillMode fill_mode = NEXT_TICK;
    Decimal   initial_capital{0};
    double    maker_fee = 0.0;    // e.g. 0.001 = 10 bps
    double    taker_fee = 0.0;
    bool      record_equity_every_tick = true;
};

// ============================================================================
// BacktestEngine — run a strategy against historical data
// ============================================================================
//
// Ties together TimeReplayer, OrderBookV2, RiskEngine, PositionManager, and
// MetricsCollector. Replays historical market data through a live strategy
// and simulates order execution.
//
// Typical usage:
//   BacktestEngine engine;
//   engine.config().fill_mode = BacktestConfig::NEXT_TICK;
//   engine.config().initial_capital = toDecimal(10000.0);
//   engine.setStrategy(std::make_unique<GridStrategy>(...));
//   engine.setData(logSet);
//   engine.run();
//   engine.metrics().exportToJSON();  // analyse results

class BacktestEngine {
public:
    BacktestEngine();
    ~BacktestEngine();

    BacktestEngine(const BacktestEngine&) = delete;
    BacktestEngine& operator=(const BacktestEngine&) = delete;

    // --- Configuration ---

    BacktestConfig& config()       { return config_; }
    const BacktestConfig& config() const { return config_; }

    // --- Strategy ---

    void setStrategy(std::unique_ptr<trading::Strategy> strategy);

    // --- Data ---

    void setData(logging::LogFileSet& logs);

    // --- Execution ---

    /// Run the backtest. Returns after all events are replayed.
    void run();

    // --- Results ---

    const MetricsCollector& metrics()   const { return metrics_; }
    const trading::PositionManager& positions() const { return pm_; }
    const market_data::OrderBookV2& orderBook() const { return book_; }

private:
    // Simulated order processing
    void drainOrderQueue();
    void simulateFillsForPending(const Tick& tick);

    // Data source helpers
    void setupReplayer(logging::LogFileSet& logs);

    BacktestConfig config_;

    // Simulated market infrastructure (same types as live)
    market_data::OrderBookV2              book_;
    trading::PositionManager             pm_;
    risk::RiskEngine                      risk_;
    trading::OrderIDGenerator             id_gen_;
    MetricsCollector                      metrics_;

    // Strategy context (friend access to wire internals)
    trading::StrategyContext              ctx_;
    std::unique_ptr<trading::Strategy>    strategy_;

    // Order queue (matches live architecture: StrategyContext pushes here)
    utils::MPMCQueue<OrderRequest, 1024>  order_queue_;
    std::atomic<uint64_t>                orders_submitted_{0};
    std::atomic<uint64_t>                orders_risk_rejected_{0};
    std::atomic<uint64_t>                orders_queue_dropped_{0};

    // Pending orders (simulated exchange latency)
    struct PendingOrder {
        OrderRequest order;
        uint64_t     placed_at_us;
    };
    std::vector<PendingOrder> pending_;

    // Time tracking
    TimeReplayer replayer_;
    uint64_t     current_tick_ts_{0};
    Tick         last_tick_;  // for NEXT_TICK fill simulation
    bool         has_last_tick_{false};

    // Capital tracking
    Decimal initial_equity_{0};
};

}  // namespace backtest
}  // namespace chronos
