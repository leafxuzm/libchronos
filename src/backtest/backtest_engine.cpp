#include "chronos/backtest/backtest_engine.hpp"
#include "chronos/core/config.hpp"

namespace chronos {
namespace backtest {

// ============================================================================
// Constructor / Destructor
// ============================================================================

BacktestEngine::BacktestEngine()
    : risk_(pm_) {
    // Wire StrategyContext internals (friend access) to simulated components
    ctx_.risk_engine_          = &risk_;
    ctx_.position_manager_     = &pm_;
    ctx_.order_id_gen_         = &id_gen_;
    ctx_.order_output_         = &order_queue_;
    ctx_.orders_submitted_     = &orders_submitted_;
    ctx_.orders_risk_rejected_ = &orders_risk_rejected_;
    ctx_.orders_queue_dropped_ = &orders_queue_dropped_;
}

BacktestEngine::~BacktestEngine() = default;

// ============================================================================
// Setup
// ============================================================================

void BacktestEngine::setStrategy(std::unique_ptr<trading::Strategy> strategy) {
    strategy_ = std::move(strategy);
}

void BacktestEngine::setData(logging::LogFileSet& logs) {
    setupReplayer(logs);
}

void BacktestEngine::setupReplayer(logging::LogFileSet& logs) {
    if (logs.tick.isOpen())  replayer_.addStream(logs.tick);
    // Orders and fills from the log are historical (live trading records).
    // The backtest engine simulates its own fills, so we only replay ticks.
}

// ============================================================================
// Run
// ============================================================================

void BacktestEngine::run() {
    if (!strategy_) return;

    // --- Initialise risk with permissive limits ---
    RiskParameters risk_params;
    risk_params.max_order_value        = 1000000.0;
    risk_params.max_position_value     = 1000000.0;
    risk_params.max_orders_per_second  = 1000000;
    risk_params.max_total_position_value = 10000000.0;
    risk_.updateParameters(risk_params);

    ctx_.current_strategy_id_ = 1;

    // --- Initial capital ---
    initial_equity_ = config_.initial_capital;
    if (initial_equity_ == Decimal(0)) initial_equity_ = toDecimal(10000.0);

    // --- Temp orderbook map (one per symbol, default symbol 1) ---
    std::unordered_map<uint32_t, market_data::OrderBookV2> books;
    books.try_emplace(1);  // construct in place (OrderBookV2 is non-movable)
    ctx_.order_books_ = &books;

    // --- Call strategy onLoad ---
    strategy_->onLoad(ctx_);

    // --- Record initial equity ---
    metrics_.recordEquity(0, initial_equity_);

    // --- Set up tick callback on replayer ---
    replayer_.setTickCallback([this](const Tick& tick) {
        current_tick_ts_ = tick.exchange_timestamp_us;
        last_tick_ = tick;
        has_last_tick_ = true;

        // Update orderbook
        book_.update(tick);

        // Keep the context-visible orderbook in sync for strategy queries
        (*ctx_.order_books_)[1].update(tick);

        // Set context timestamp before calling strategy
        ctx_.timestamp_us_ = current_tick_ts_;

        // Feed tick to strategy
        strategy_->onTick(tick, ctx_);

        // Process orders the strategy submitted
        drainOrderQueue();

        // Simulate fills at current price
        simulateFillsForPending(tick);

        // Calculate current equity (initial capital + total P&L)
        Decimal total_pnl{0};
        for (auto& pos : pm_.getAllPositions()) {
            // Estimate unrealised P&L at current price
            auto best_bid = book_.getBestBid();
            auto best_ask = book_.getBestAsk();
            Decimal mark_price = tick.price;  // use last trade price as mark
            if (pos.quantity > Decimal(0) && best_bid.has_value())
                mark_price = *best_bid;
            else if (pos.quantity < Decimal(0) && best_ask.has_value())
                mark_price = *best_ask;
            total_pnl = total_pnl + pos.getTotalPnL(mark_price);
        }

        if (config_.record_equity_every_tick) {
            metrics_.recordEquity(current_tick_ts_, initial_equity_ + total_pnl);
        }
    });

    // Ignore orders and fills from the log — we simulate our own fills.
    replayer_.setOrderCallback([](const OrderRequest&) {});
    replayer_.setFillCallback([](const Fill&) {});

    // --- Replay loop ---
    while (!replayer_.isExhausted()) {
        if (!replayer_.advanceToNextEvent()) break;
    }

    // --- Final equity ---
    // (already recorded via last tick callback)

    // --- Compute metrics ---
    metrics_.calculateMetrics();
}

// ============================================================================
// Simulated Order Processing
// ============================================================================

void BacktestEngine::drainOrderQueue() {
    OrderRequest order;
    while (order_queue_.try_pop(order)) {
        // Risk check (redundant with submitOrder but safe double-check)
        if (!risk_.checkOrder(order).passed) {
            orders_risk_rejected_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        // order_id already set by StrategyContext::submitOrder()
        order.timestamp_us = current_tick_ts_;

        // Add to pending for fill simulation
        pending_.push_back({order, current_tick_ts_});
    }
}

void BacktestEngine::simulateFillsForPending(const Tick& tick) {
    auto it = pending_.begin();
    while (it != pending_.end()) {
        bool filled = false;
        Fill f;

        switch (config_.fill_mode) {
            case BacktestConfig::IMMEDIATE:
            case BacktestConfig::NEXT_TICK:
            case BacktestConfig::CONSERVATIVE: {
                // All modes: use current tick to determine fill price
                auto best_bid = book_.getBestBid();
                auto best_ask = book_.getBestAsk();
                Decimal fill_price = tick.price;  // fallback: last trade price

                if (it->order.side == OrderSide::BUY) {
                    if (config_.fill_mode == BacktestConfig::CONSERVATIVE) {
                        fill_price = best_ask.value_or(tick.price);
                    } else {
                        fill_price = best_ask.value_or(tick.price);
                    }
                } else {
                    if (config_.fill_mode == BacktestConfig::CONSERVATIVE) {
                        fill_price = best_bid.value_or(tick.price);
                    } else {
                        fill_price = best_bid.value_or(tick.price);
                    }
                }

                // Don't fill if there's no liquidity
                if (it->order.side == OrderSide::BUY && !best_ask.has_value()) {
                    ++it; continue;
                }
                if (it->order.side == OrderSide::SELL && !best_bid.has_value()) {
                    ++it; continue;
                }

                f.order_id = it->order.order_id;
                f.fill_price = fill_price;
                f.fill_quantity = it->order.quantity;
                f.side = it->order.side;
                f.symbol_id = it->order.symbol_id;
                f.exchange_timestamp_us = tick.exchange_timestamp_us;
                f.strategy_id = ctx_.current_strategy_id_;
                filled = true;
                break;
            }
        }

        if (filled) {
            // Update position
            pm_.updatePosition(f);

            // Calculate realised P&L
            Trade trade;
            trade.entry_time_us = it->placed_at_us;
            trade.exit_time_us = tick.exchange_timestamp_us;
            trade.symbol_id = f.symbol_id;
            trade.entry_price = it->order.price;
            trade.exit_price = f.fill_price;
            trade.quantity = f.fill_quantity;
            trade.direction = f.side;

            if (f.side == OrderSide::BUY) {
                trade.pnl = f.fill_quantity * (f.fill_price - it->order.price);
            } else {
                trade.pnl = f.fill_quantity * (it->order.price - f.fill_price);
            }

            // Apply taker fee
            if (config_.taker_fee > 0.0) {
                Decimal fee = f.fill_price * f.fill_quantity * toDecimal(config_.taker_fee);
                trade.pnl = trade.pnl - fee;
            }

            metrics_.recordTrade(trade);

            // Notify strategy
            strategy_->onFill(f, ctx_);

            it = pending_.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace backtest
}  // namespace chronos
