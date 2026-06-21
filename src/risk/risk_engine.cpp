#include "chronos/risk/risk_engine.hpp"
#include "chronos/core/error.hpp"
#include <algorithm>
#include <chrono>

namespace chronos {
namespace risk {

namespace {

inline uint64_t now_us() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

inline uint32_t current_second_since_epoch() {
    return static_cast<uint32_t>(now_us() / 1'000'000);
}

/// C++20 spinlock using std::atomic_flag (stats updates only, rarely contended).
inline void spin_acquire(std::atomic_flag& lock) {
    while (lock.test_and_set(std::memory_order_acquire)) { /* spin */ }
}

inline void spin_release(std::atomic_flag& lock) {
    lock.clear(std::memory_order_release);
}

}  // anonymous namespace

// ============================================================================
// Constructor
// ============================================================================

RiskEngine::RiskEngine(trading::PositionManager& position_manager)
    : position_manager_(position_manager)
{
    RiskParameters defaults;
    for (auto& buf : cold_buffers_) {
        buf.params = defaults;
        buf.buildHot();
    }
    std::memcpy(&hot_params_, &cold_buffers_[0].hot, offsetof(HotParams, version));
    hot_params_.version.store(0, std::memory_order_relaxed);
    setAvailableCapital(toDecimal(defaults.max_total_position_value));
}

// ============================================================================
// Configuration
// ============================================================================

void RiskEngine::updateParameters(const RiskParameters& params) {
    ColdParams* dark = getWriteBuffer();
    dark->params = params;
    dark->update_time_us = now_us();
    publishParams();
}

RiskParameters RiskEngine::getParameters() const {
    return getReadBuffer()->params;
}

// ============================================================================
// Rate Limiter
// ============================================================================

bool RiskEngine::checkRateLimitInternal() const {
    uint32_t now_sec = current_second_since_epoch();
    uint32_t prev_sec = current_second_.load(std::memory_order_relaxed);

    if (prev_sec != now_sec) {
        if (current_second_.compare_exchange_strong(
                prev_sec, now_sec, std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            orders_this_second_.store(0, std::memory_order_relaxed);
        }
    }

    uint32_t max_ops = readHotParamsOptimistic(
        [](const HotParams& hp) { return hp.max_orders_per_second; });

    uint32_t count = orders_this_second_.fetch_add(1, std::memory_order_relaxed);
    return count < max_ops;
}

bool RiskEngine::checkRateLimit() const {
    return checkRateLimitInternal();
}

// ============================================================================
// checkOrder — the hot path
// ============================================================================

RiskCheckResult RiskEngine::checkOrder(const OrderRequest& order) const {
    auto t_start = std::chrono::high_resolution_clock::now();

    // Calculate order value once
    Decimal order_qty = order.quantity;
    Decimal order_value = order.price * order_qty;
    Decimal order_value_abs = order_value < Decimal(0) ? -order_value : order_value;

    // Read hot params optimistically and gather rejection data
    struct CheckInfo {
        Decimal max_order_value;
        Decimal max_position_value;
        Decimal max_total_position_value;
        Decimal min_available_capital;
        uint64_t version;
    };

    auto info = readHotParamsOptimistic([](const HotParams& hp) -> CheckInfo {
        return {hp.max_order_value, hp.max_position_value,
                hp.max_total_position_value, hp.min_available_capital, 0};
    });

    RiskCheckResult result = RiskCheckResult::accept();
    result.check_timestamp_us = now_us();

    // 1. Order value check
    if (order_value_abs > info.max_order_value) {
        result = RiskCheckResult::reject("Order value exceeds limit");
    }

    // 2. Rate limit check
    if (result.passed && !checkRateLimitInternal()) {
        result = RiskCheckResult::reject("Rate limit exceeded");
    }

    // 3. Position limit check (per symbol)
    if (result.passed) {
        const Position* existing = position_manager_.getPosition(order.symbol_id);
        Decimal existing_qty = existing ? existing->quantity : Decimal(0);
        Decimal signed_qty = (order.side == OrderSide::SELL)
            ? -order_qty : order_qty;
        Decimal projected_qty = existing_qty + signed_qty;
        Decimal projected_abs = projected_qty < Decimal(0)
            ? -projected_qty : projected_qty;
        Decimal projected_value = projected_abs * order.price;

        if (projected_value > info.max_position_value) {
            result = RiskCheckResult::reject(
                "Position value for symbol " + std::to_string(order.symbol_id) +
                " would exceed limit");
        }
    }

    // 4. Total position value check
    if (result.passed) {
        Decimal cached_total = position_manager_.getTotalValueCached();
        if (cached_total > info.max_total_position_value) {
            result = RiskCheckResult::reject("Total position value exceeds limit");
        }
    }

    // 5. Capital sufficiency check
    if (result.passed) {
        Decimal capital = Decimal::from_raw_value(
            available_capital_raw_.load(std::memory_order_acquire));
        Decimal remaining = capital - order_value_abs;
        if (remaining < info.min_available_capital) {
            result = RiskCheckResult::reject("Insufficient available capital");
        }
    }

    // Update statistics
    auto t_end = std::chrono::high_resolution_clock::now();
    auto latency_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        t_end - t_start).count();
    updateStats(result.passed, result.rejection_reason,
                static_cast<uint64_t>(latency_ns));

    return result;
}

// ============================================================================
// Capital Management
// ============================================================================

void RiskEngine::setAvailableCapital(Decimal capital) {
    available_capital_raw_.store(capital.raw_value(),
                                 std::memory_order_release);
}

Decimal RiskEngine::getAvailableCapital() const {
    return Decimal::from_raw_value(
        available_capital_raw_.load(std::memory_order_acquire));
}

// ============================================================================
// Statistics
// ============================================================================

void RiskEngine::updateStats(bool passed, const std::string& reason,
                              uint64_t latency_ns) const
{
    spin_acquire(stats_lock_);

    stats_.total_checks++;
    if (passed) {
        stats_.accepted++;
    } else {
        stats_.rejected++;
        if (reason.find("Rate limit") != std::string::npos) {
            stats_.rate_limit_rejects++;
        } else if (reason.find("Order value") != std::string::npos) {
            stats_.order_value_rejects++;
        } else if (reason.find("Position") != std::string::npos) {
            stats_.position_limit_rejects++;
        } else if (reason.find("Total position") != std::string::npos) {
            stats_.total_position_rejects++;
        } else if (reason.find("capital") != std::string::npos ||
                   reason.find("Capital") != std::string::npos) {
            stats_.capital_rejects++;
        }
    }

    // Exponential moving average for latency
    auto& avg = stats_.avg_check_latency;
    if (avg == std::chrono::nanoseconds::zero()) {
        avg = std::chrono::nanoseconds(latency_ns);
    } else {
        avg = std::chrono::nanoseconds(
            (avg.count() * 7 + static_cast<int64_t>(latency_ns)) / 8);
    }

    spin_release(stats_lock_);
}

RiskEngine::Statistics RiskEngine::getStatistics() const {
    spin_acquire(stats_lock_);
    auto copy = stats_;
    spin_release(stats_lock_);
    return copy;
}

void RiskEngine::resetStatistics() {
    spin_acquire(stats_lock_);
    stats_ = Statistics{};
    spin_release(stats_lock_);
}

}  // namespace risk
}  // namespace chronos
