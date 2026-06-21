#pragma once

#include "chronos/core/types.hpp"
#include "chronos/trading/position_manager.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>

namespace chronos {
namespace execution {

// ============================================================================
// Notification callbacks
// ============================================================================

using FillNotifier   = std::function<void(const Fill&)>;
using AckNotifier    = std::function<void(const OrderAck&)>;
using RejectNotifier = std::function<void(const OrderReject&)>;

// ============================================================================
// Statistics
// ============================================================================

struct ExecutionHandlerStats {
    uint64_t fills_processed{0};
    uint64_t acks_processed{0};
    uint64_t rejects_processed{0};
    uint64_t avg_latency_ns{0};  // EMA of onFill wall-clock time
};

// ============================================================================
// ExecutionHandler — fill processing and order lifecycle management
// ============================================================================

class ExecutionHandler {
public:
    /// @param pm             PositionManager for position updates (may be nullptr)
    /// @param fill_notifier  Called after position update (e.g. → StrategyEngine)
    /// @param ack_notifier   Called on order acknowledgment
    /// @param reject_notifier Called on order rejection
    ExecutionHandler(trading::PositionManager* pm,
                     FillNotifier fill_notifier = nullptr,
                     AckNotifier ack_notifier = nullptr,
                     RejectNotifier reject_notifier = nullptr);

    ExecutionHandler(const ExecutionHandler&) = delete;
    ExecutionHandler& operator=(const ExecutionHandler&) = delete;

    // --- Event handlers ---

    /// Process a fill: update position → log → notify.
    /// Thread-safe.  Target <100μs.
    void onFill(const Fill& fill);

    /// Process an order acknowledgment.
    void onOrderAck(const OrderAck& ack);

    /// Process an order rejection.
    void onOrderReject(const OrderReject& reject);

    // --- Queries ---

    trading::PositionManager* positionManager() const { return pm_; }

    ExecutionHandlerStats getStats() const;
    void resetStats();

private:
    trading::PositionManager* pm_;

    FillNotifier   fill_notifier_;
    AckNotifier    ack_notifier_;
    RejectNotifier reject_notifier_;

    mutable std::mutex stats_mutex_;
    ExecutionHandlerStats stats_;

    // Latency EMA: (old * 7 + new) / 8
    std::atomic<uint64_t> avg_latency_ns_{0};
};

}  // namespace execution
}  // namespace chronos
