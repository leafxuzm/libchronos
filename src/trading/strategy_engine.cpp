#include "chronos/trading/strategy_engine.hpp"
#include "chronos/utils/cpu_affinity.hpp"
#include <thread>

namespace chronos {
namespace trading {

// ============================================================================
// StrategyContext
// ============================================================================

std::optional<Decimal> StrategyContext::getBestBid(uint32_t symbol_id) const {
    auto it = order_books_->find(symbol_id);
    if (it == order_books_->end()) return std::nullopt;
    return it->second.getBestBid();
}

std::optional<Decimal> StrategyContext::getBestAsk(uint32_t symbol_id) const {
    auto it = order_books_->find(symbol_id);
    if (it == order_books_->end()) return std::nullopt;
    return it->second.getBestAsk();
}

std::optional<Decimal> StrategyContext::getMidPrice(uint32_t symbol_id) const {
    auto it = order_books_->find(symbol_id);
    if (it == order_books_->end()) return std::nullopt;
    return it->second.getMidPrice();
}

std::array<PriceLevel, 5> StrategyContext::getTop5Bids(uint32_t symbol_id) const {
    auto it = order_books_->find(symbol_id);
    if (it == order_books_->end()) return {};
    return it->second.getTop5Bids();
}

std::array<PriceLevel, 5> StrategyContext::getTop5Asks(uint32_t symbol_id) const {
    auto it = order_books_->find(symbol_id);
    if (it == order_books_->end()) return {};
    return it->second.getTop5Asks();
}

const Position* StrategyContext::getPosition(uint32_t symbol_id) const {
    return position_manager_->getPosition(symbol_id);
}

Decimal StrategyContext::getAvailableCapital() const {
    return risk_engine_->getAvailableCapital();
}

uint64_t StrategyContext::submitOrder(const OrderRequest& order) {
    auto result = risk_engine_->checkOrder(order);
    if (!result.passed) {
        if (orders_risk_rejected_) orders_risk_rejected_->fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    OrderRequest validated = order;
    validated.order_id      = order_id_gen_->generate();
    validated.timestamp_us  = timestamp_us_;
    validated.strategy_id   = current_strategy_id_;

    if (!order_output_->try_push(validated)) {
        if (orders_queue_dropped_) orders_queue_dropped_->fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    if (orders_submitted_) orders_submitted_->fetch_add(1, std::memory_order_relaxed);
    return validated.order_id;
}

bool StrategyContext::cancelOrder(uint64_t order_id) {
    OrderRequest cancel;
    cancel.order_id = order_id;
    cancel.quantity = Decimal(0);  // zero quantity signals cancellation
    cancel.timestamp_us = timestamp_us_;
    return order_output_->try_push(cancel);
}

// ============================================================================
// StrategyEngine
// ============================================================================

StrategyEngine::StrategyEngine()
    : risk_engine_(position_manager_)
{
    // Wire the context to our components
    ctx_.order_books_      = &order_books_;
    ctx_.risk_engine_      = &risk_engine_;
    ctx_.position_manager_ = &position_manager_;
    ctx_.order_id_gen_     = &order_id_gen_;
    ctx_.order_output_     = &order_queue_;
    ctx_.orders_submitted_      = &orders_submitted_;
    ctx_.orders_risk_rejected_  = &orders_risk_rejected_;
    ctx_.orders_queue_dropped_  = &orders_queue_dropped_;
}

StrategyEngine::~StrategyEngine() {
    stop();
}

void StrategyEngine::start() {
    if (running_.load(std::memory_order_acquire)) return;

    // Call onLoad for each strategy
    for (auto& entry : strategies_) {
        entry.strategy->onLoad(ctx_);
    }

    running_.store(true, std::memory_order_release);
    worker_ = std::thread(&StrategyEngine::run, this);
}

void StrategyEngine::stop() {
    if (!running_.load(std::memory_order_acquire)) return;

    running_.store(false, std::memory_order_release);
    if (worker_.joinable()) {
        worker_.join();
    }

    // Call onUnload for each strategy
    for (auto& entry : strategies_) {
        entry.strategy->onUnload(ctx_);
    }
}

void StrategyEngine::registerStrategy(std::unique_ptr<Strategy> strategy) {
    auto syms = strategy->symbols();
    bool wildcard = syms.empty();
    std::unordered_set<uint32_t> sym_set(syms.begin(), syms.end());
    uint32_t id = static_cast<uint32_t>(strategies_.size()) + 1;
    strategies_.push_back({id, std::move(strategy), std::move(sym_set), wildcard});
    strategy_by_id_[id] = strategies_.back().strategy.get();
}

StrategyEngine::Stats StrategyEngine::getStats() const {
    Stats s;
    s.ticks_processed       = ticks_processed_.load(std::memory_order_relaxed);
    s.fills_processed       = fills_processed_.load(std::memory_order_relaxed);
    s.orders_submitted      = orders_submitted_.load(std::memory_order_relaxed);
    s.orders_risk_rejected  = orders_risk_rejected_.load(std::memory_order_relaxed);
    s.orders_queue_dropped  = orders_queue_dropped_.load(std::memory_order_relaxed);
    return s;
}

// ============================================================================
// Main dispatch loop
// ============================================================================

void StrategyEngine::run() {
    if (cpu_affinity_ >= 0) {
        utils::setCpuAffinity(cpu_affinity_);
    }

    Tick tick;
    Fill fill;
    uint64_t last_timer_us = 0;
    constexpr uint64_t TIMER_INTERVAL_US = 100000;  // 100ms

    while (running_.load(std::memory_order_acquire)) {
        bool did_work = false;

        // --- Tick processing ---
        if (tick_queue_.try_pop(tick)) {
            did_work = true;
            ticks_processed_.fetch_add(1, std::memory_order_relaxed);
            ctx_.timestamp_us_ = tick.receive_timestamp_us;

            auto [it, inserted] = order_books_.try_emplace(tick.symbol_id);
            (void)inserted;
            it->second.update(tick);

            for (auto& entry : strategies_) {
                if (entry.is_wildcard || entry.symbols.count(tick.symbol_id)) {
                    ctx_.current_strategy_id_ = entry.id;
                    entry.strategy->onTick(tick, ctx_);
                }
            }

            if (tick.receive_timestamp_us - last_timer_us > TIMER_INTERVAL_US) {
                last_timer_us = tick.receive_timestamp_us;
                for (auto& entry : strategies_) {
                    ctx_.current_strategy_id_ = entry.id;
                    entry.strategy->onTimer(tick.receive_timestamp_us, ctx_);
                }
            }
        }

        // --- Fill processing ---
        if (fill_queue_.try_pop(fill)) {
            did_work = true;
            fills_processed_.fetch_add(1, std::memory_order_relaxed);
            ctx_.timestamp_us_ = fill.receive_timestamp_us;

            // Update position from fill
            position_manager_.updatePosition(fill);

            // Route onFill to the owning strategy
            auto sit = strategy_by_id_.find(fill.strategy_id);
            if (sit != strategy_by_id_.end()) {
                ctx_.current_strategy_id_ = fill.strategy_id;
                sit->second->onFill(fill, ctx_);
            }
        }

        // --- Idle timer ---
        if (!did_work) {
            static auto last_idle_timer = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_idle_timer).count();
            if (elapsed_ms >= 100) {
                last_idle_timer = now;
                uint64_t now_us = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        now.time_since_epoch()).count());
                for (auto& entry : strategies_) {
                    ctx_.current_strategy_id_ = entry.id;
                    entry.strategy->onTimer(now_us, ctx_);
                }
            }
            utils::cpuRelax();
        }
    }
}

}  // namespace trading
}  // namespace chronos
