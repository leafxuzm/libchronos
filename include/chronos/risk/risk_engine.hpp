#pragma once

#include "chronos/core/types.hpp"
#include "chronos/core/config.hpp"
#include "chronos/trading/position_manager.hpp"
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <chrono>

namespace chronos {
namespace risk {

/// Result of a single pre-trade risk check.
struct alignas(64) RiskCheckResult {
    bool passed;
    std::string rejection_reason;
    uint64_t check_timestamp_us;

    RiskCheckResult()
        : passed(true), check_timestamp_us(0) {}

    static RiskCheckResult accept() {
        RiskCheckResult r;
        r.passed = true;
        return r;
    }

    static RiskCheckResult reject(std::string reason) {
        RiskCheckResult r;
        r.passed = false;
        r.rejection_reason = std::move(reason);
        return r;
    }
};

// ============================================================================
// RiskEngine
// ============================================================================

/// Pre-trade risk engine with hot/cold data separation.
///
/// HotParams (256-byte aligned) holds Decimal-converted risk parameters with an
/// atomic version counter for optimistic lock-free reads.  Parameters change
/// rarely (on config reload), so this is a read-mostly optimization.
///
/// ColdParams uses double-buffering: writer updates the dark buffer, then
/// atomically swaps the pointer — RCU-style, no reader blocking.
class RiskEngine {
public:
    explicit RiskEngine(trading::PositionManager& position_manager);

    RiskEngine(const RiskEngine&) = delete;
    RiskEngine& operator=(const RiskEngine&) = delete;

    // --- Configuration ---

    /// Thread-safe parameter update (swaps cold buffer, increments version).
    void updateParameters(const RiskParameters& params);

    /// Snapshot of current parameters.
    RiskParameters getParameters() const;

    // --- Risk Checks (hot path, target <1us) ---

    /// Check all 5 risk limits for an order:
    ///   1. Order value  : |qty * price| <= max_order_value
    ///   2. Position limit: projected symbol position <= max_position_value
    ///   3. Rate limit   : orders in current second < max_orders_per_second
    ///   4. Total position: portfolio value <= max_total_position_value
    ///   5. Capital      : available_capital - order_value >= min_capital
    RiskCheckResult checkOrder(const OrderRequest& order) const;

    /// Lightweight rate-limit-only check (use as pre-filter before full check).
    bool checkRateLimit() const;

    // --- Capital ---

    void setAvailableCapital(Decimal capital);
    Decimal getAvailableCapital() const;

    // --- Statistics ---

    struct Statistics {
        uint64_t total_checks = 0;
        uint64_t accepted = 0;
        uint64_t rejected = 0;
        uint64_t rate_limit_rejects = 0;
        uint64_t order_value_rejects = 0;
        uint64_t position_limit_rejects = 0;
        uint64_t total_position_rejects = 0;
        uint64_t capital_rejects = 0;
        std::chrono::nanoseconds avg_check_latency{0};
    };

    Statistics getStatistics() const;
    void resetStatistics();

private:
    // ========================================================================
    // HotParams — cached, Decimal-converted, cache-line friendly
    // ========================================================================

    struct alignas(256) HotParams {
        Decimal max_order_value{0};
        Decimal max_position_value{0};
        Decimal max_total_position_value{0};
        Decimal min_available_capital{0};
        uint32_t max_orders_per_second{100};
        std::atomic<uint64_t> version{0};   // seqlock version (odd = write in progress)
        uint8_t padding[204];               // pad to 256 bytes
    };

    static_assert(sizeof(HotParams) == 256, "HotParams must be 256 bytes");
    static_assert(alignof(HotParams) == 256, "HotParams must be 256-byte aligned");

    // ========================================================================
    // ColdParams — double-buffered (original + pre-converted Decimal form)
    // ========================================================================

    struct ColdParams {
        RiskParameters params;          // original double-based config
        HotParams hot;                  // pre-converted Decimal form
        uint64_t update_time_us{0};

        void buildHot() {
            hot.max_order_value           = toDecimal(params.max_order_value);
            hot.max_position_value        = toDecimal(params.max_position_value);
            hot.max_total_position_value  = toDecimal(params.max_total_position_value);
            hot.min_available_capital     = toDecimal(params.min_available_capital);
            hot.max_orders_per_second     = params.max_orders_per_second;
        }
    };

    // ========================================================================
    // Members
    // ========================================================================

    mutable HotParams hot_params_;

    ColdParams cold_buffers_[2];
    std::atomic<const ColdParams*> active_cold_{&cold_buffers_[0]};
    size_t write_idx_{1};  // dark buffer index

    // Rate limiter: atomic second counter + CAS-based boundary detection
    mutable std::atomic<uint32_t> current_second_{0};
    mutable std::atomic<uint32_t> orders_this_second_{0};

    // Available capital (raw Decimal value for lock-free atomic access)
    mutable std::atomic<int64_t> available_capital_raw_{0};

    trading::PositionManager& position_manager_;

    mutable Statistics stats_;
    mutable std::atomic_flag stats_lock_ = ATOMIC_FLAG_INIT;  // C++20 spinlock

    // ========================================================================
    // Internal helpers
    // ========================================================================

    template<typename Func>
    auto readHotParamsOptimistic(Func&& func) const -> decltype(func(hot_params_));

    const ColdParams* getReadBuffer() const {
        return active_cold_.load(std::memory_order_acquire);
    }

    ColdParams* getWriteBuffer() {
        return &cold_buffers_[write_idx_];
    }

    void swapColdBuffers() {
        active_cold_.store(getWriteBuffer(), std::memory_order_release);
        write_idx_ = (write_idx_ == 0) ? 1 : 0;

        ColdParams* next = getWriteBuffer();
        std::memcpy(&next->hot, &hot_params_, offsetof(HotParams, version));
    }

    void publishParams() {
        ColdParams* dark = getWriteBuffer();
        dark->buildHot();
        dark->hot.version.store(
            hot_params_.version.load(std::memory_order_relaxed) + 1,
            std::memory_order_relaxed);

        // Copy hot form back to shared hot_params_ and bump version
        hot_params_.version.store(
            hot_params_.version.load(std::memory_order_relaxed) + 1,
            std::memory_order_release);  // start write (odd)
        std::memcpy(&hot_params_, &dark->hot, offsetof(HotParams, version));
        hot_params_.version.store(
            hot_params_.version.load(std::memory_order_relaxed) + 1,
            std::memory_order_release);  // end write (even)

        swapColdBuffers();
    }

    bool checkRateLimitInternal() const;
    void updateStats(bool passed, const std::string& reason,
                     uint64_t latency_ns) const;
};

// ============================================================================
// Template — optimistic read (seqlock pattern, same as OrderBookV2)
// ============================================================================

template<typename Func>
auto RiskEngine::readHotParamsOptimistic(Func&& func) const
    -> decltype(func(hot_params_))
{
    constexpr int MAX_RETRIES = 100;

    for (int retry = 0; retry < MAX_RETRIES; ++retry) {
        uint64_t v1 = hot_params_.version.load(std::memory_order_acquire);
        if (v1 & 1) continue;  // writer in progress — retry

        auto result = func(hot_params_);

        uint64_t v2 = hot_params_.version.load(std::memory_order_acquire);
        if (v1 == v2) return result;  // consistent read
    }
    // Fallback: read anyway (reader won't see corruption, just possibly stale)
    return func(hot_params_);
}

}  // namespace risk
}  // namespace chronos
