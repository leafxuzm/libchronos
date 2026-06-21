#pragma once

#include "chronos/core/types.hpp"
#include "chronos/utils/mpmc_queue.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace chronos {
namespace logging {

// ============================================================================
// ZMQConfig
// ============================================================================

struct ZMQConfig {
    std::string bind_address = "tcp://*:5555";
    int send_hwm = 10000;            // drop old messages when subscriber is slow
    int linger_ms = 0;               // don't block on close
    size_t queue_capacity = 65536;   // internal event queue size
};

// ============================================================================
// ZMQBridge — fan-out engine events to external processes via ZMQ PUB-SUB
// ============================================================================
//
// Hot path: publishTick/Order/Fill enqueue into lock-free MPMC queue and
// return immediately. A dedicated publisher thread drains the queue and
// sends binary-encoded messages over a ZMQ PUB socket.
//
// Wire format per message: [4B topic][N-byte binary record]
//   Topic "TICK" + 64B Tick
//   Topic "ORDR" + 128B OrderRequest
//   Topic "FILL" + 128B Fill

class ZMQBridge {
public:
    ZMQBridge() = default;
    ~ZMQBridge();

    ZMQBridge(const ZMQBridge&) = delete;
    ZMQBridge& operator=(const ZMQBridge&) = delete;

    // --- Lifecycle ---

    bool initialize(const ZMQConfig& config);
    void stop();
    bool isRunning() const { return running_.load(std::memory_order_acquire); }

    // --- Hot path (non-blocking) ---

    bool publishTick(const Tick& tick);
    bool publishOrder(const OrderRequest& order);
    bool publishFill(const Fill& fill);

    // --- Statistics ---

    uint64_t ticksPublished()  const { return ticks_pub_.load(std::memory_order_relaxed); }
    uint64_t ordersPublished() const { return orders_pub_.load(std::memory_order_relaxed); }
    uint64_t fillsPublished()  const { return fills_pub_.load(std::memory_order_relaxed); }
    uint64_t droppedCount()    const { return dropped_.load(std::memory_order_relaxed); }

private:
    void run();

    // Event type tag for the internal queue
    enum class EventTag : uint8_t { TICK = 0, ORDER = 1, FILL = 2 };

    // Event for the internal queue — flat byte storage for trivial copyability.
    // Tick=64B, OrderRequest=128B, Fill=128B → max 128B.
    static constexpr size_t EVENT_DATA_SIZE = 128;

    struct Event {
        EventTag tag;
        alignas(64) uint8_t data[EVENT_DATA_SIZE];
    };

    ZMQConfig config_;
    void* zmq_context_{nullptr};
    void* zmq_socket_{nullptr};

    // Heap-allocated: MPMCQueue with 65536 × ~136B ≈ 9 MB, too large for stack.
    using Queue = utils::MPMCQueue<Event, 65536>;
    std::unique_ptr<Queue> queue_;
    std::thread pub_thread_;
    std::atomic<bool> running_{false};

    std::atomic<uint64_t> ticks_pub_{0};
    std::atomic<uint64_t> orders_pub_{0};
    std::atomic<uint64_t> fills_pub_{0};
    std::atomic<uint64_t> dropped_{0};
};

}  // namespace logging
}  // namespace chronos
