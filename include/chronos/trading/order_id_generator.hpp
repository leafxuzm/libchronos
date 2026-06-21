#pragma once

#include <atomic>
#include <cstdint>

namespace chronos {
namespace trading {

/// Lock-free monotonic order ID generator.
/// Performance target: <10ns per generate().
class OrderIDGenerator {
public:
    OrderIDGenerator()
        : current_id_(1) {}  // 0 reserved for invalid/unset

    explicit OrderIDGenerator(uint64_t start_id)
        : current_id_(start_id) {}

    // Non-copyable, non-movable (atomic member)
    OrderIDGenerator(const OrderIDGenerator&) = delete;
    OrderIDGenerator& operator=(const OrderIDGenerator&) = delete;

    /// Generate a new unique order ID.
    uint64_t generate() {
        return current_id_.fetch_add(1, std::memory_order_relaxed);
    }

    /// Peek at the next ID without consuming it (for persistence savepoints).
    uint64_t getCurrentID() const {
        return current_id_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<uint64_t> current_id_;
};

}  // namespace trading
}  // namespace chronos
