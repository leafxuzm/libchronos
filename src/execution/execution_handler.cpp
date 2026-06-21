#include "chronos/execution/execution_handler.hpp"
#include <chrono>

namespace chronos {
namespace execution {

// ============================================================================
// Helper
// ============================================================================

namespace {
uint64_t now_ns() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}
}  // namespace

// ============================================================================
// Constructor
// ============================================================================

ExecutionHandler::ExecutionHandler(trading::PositionManager* pm,
                                   FillNotifier fill_notifier,
                                   AckNotifier ack_notifier,
                                   RejectNotifier reject_notifier)
    : pm_(pm)
    , fill_notifier_(std::move(fill_notifier))
    , ack_notifier_(std::move(ack_notifier))
    , reject_notifier_(std::move(reject_notifier))
{}

// ============================================================================
// Event handlers
// ============================================================================

void ExecutionHandler::onFill(const Fill& fill) {
    uint64_t t0 = now_ns();

    // Update position
    if (pm_) {
        pm_->updatePosition(fill);
    }

    // Notify downstream (e.g. StrategyEngine for strategy callback)
    if (fill_notifier_) {
        fill_notifier_(fill);
    }

    // Update stats
    uint64_t elapsed = now_ns() - t0;
    uint64_t old_avg = avg_latency_ns_.load(std::memory_order_relaxed);
    uint64_t new_avg = (old_avg == 0) ? elapsed : (old_avg * 7 + elapsed) / 8;
    avg_latency_ns_.store(new_avg, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lk(stats_mutex_);
        stats_.fills_processed++;
        stats_.avg_latency_ns = new_avg;
    }
}

void ExecutionHandler::onOrderAck(const OrderAck& ack) {
    if (ack_notifier_) {
        ack_notifier_(ack);
    }

    std::lock_guard<std::mutex> lk(stats_mutex_);
    stats_.acks_processed++;
}

void ExecutionHandler::onOrderReject(const OrderReject& reject) {
    if (reject_notifier_) {
        reject_notifier_(reject);
    }

    std::lock_guard<std::mutex> lk(stats_mutex_);
    stats_.rejects_processed++;
}

// ============================================================================
// Statistics
// ============================================================================

ExecutionHandlerStats ExecutionHandler::getStats() const {
    std::lock_guard<std::mutex> lk(stats_mutex_);
    ExecutionHandlerStats s = stats_;
    s.avg_latency_ns = avg_latency_ns_.load(std::memory_order_relaxed);
    return s;
}

void ExecutionHandler::resetStats() {
    std::lock_guard<std::mutex> lk(stats_mutex_);
    stats_ = ExecutionHandlerStats{};
    avg_latency_ns_.store(0, std::memory_order_relaxed);
}

}  // namespace execution
}  // namespace chronos
