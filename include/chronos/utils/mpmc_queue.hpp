#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace chronos {
namespace utils {

/**
 * @brief Multi-Producer Multi-Consumer lock-free bounded queue
 *
 * Ticket-based algorithm with per-slot sequence numbers, preventing
 * wrap-around overwrite when producers temporarily outpace consumers.
 *
 * Enqueue: ticket = head.fetch_add(1), spin until seq[ticket%N] == ticket,
 *          write data, seq[ticket%N] = ticket + 1
 * Dequeue: ticket = tail.fetch_add(1), spin until seq[ticket%N] == ticket + 1,
 *          read data, seq[ticket%N] = ticket + N
 *
 * @tparam T Element type (must be trivially copyable)
 * @tparam N Queue capacity (must be power of 2)
 */
template <typename T, size_t N>
class MPMCQueue {
    static_assert((N & (N - 1)) == 0, "N must be a power of 2");
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");

private:
    // 这个必须声明 为 64 字节对齐，否则性能会差
    struct alignas(64) Slot {
        T data;
        std::atomic<size_t> seq{0};
    };

    // Slot 的声明 alignas(64) 声明 可以省略。因为 Slot 是一个结构体，已经对齐了。
    alignas(64) Slot slots_[N];
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};

public:
    MPMCQueue() {
        for (size_t i = 0; i < N; ++i) {
            slots_[i].seq.store(i, std::memory_order_relaxed);
        }
    }

    MPMCQueue(const MPMCQueue&) = delete;
    MPMCQueue& operator=(const MPMCQueue&) = delete;

    /**
     * @brief Attempt to push an element (non-blocking)
     * @return true if pushed, false if queue is full
     */
    bool try_push(const T& val) {
        size_t pos = head_.load(std::memory_order_relaxed);
        for (;;) {
            Slot& s = slots_[pos & (N - 1)];
            size_t seq = s.seq.load(std::memory_order_acquire);
            int64_t diff = static_cast<int64_t>(seq) - static_cast<int64_t>(pos);
            if (diff == 0) {
                if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    s.data = val;
                    s.seq.store(pos + 1, std::memory_order_release);
                    return true;
                }
                // If CAS fails, 'pos' is automatically updated to the latest 'head_'.
                // Continue to the next iteration to re-evaluate with the new 'pos'.
            } else if (diff < 0) {
                return false;  // full: slot not yet consumed
            } else {
                pos = head_.load(std::memory_order_relaxed);
            }
        }
    }

    /**
     * @brief Attempt to pop an element (non-blocking)
     * @return true if popped, false if queue is empty
     */
    bool try_pop(T& val) {
        size_t pos = tail_.load(std::memory_order_relaxed);
        for (;;) {
            Slot& s = slots_[pos & (N - 1)];
            size_t seq = s.seq.load(std::memory_order_acquire);
            int64_t diff = static_cast<int64_t>(seq) - static_cast<int64_t>(pos + 1);
            if (diff == 0) {
                if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    val = s.data;
                    s.seq.store(pos + N, std::memory_order_release);
                    return true;
                }
                // If CAS fails, 'pos' is automatically updated to the latest 'tail_'.
                // Continue to the next iteration to re-evaluate with the new 'pos'.
            } else if (diff < 0) {
                return false;  // empty: no data written yet
            } else {
                pos = tail_.load(std::memory_order_relaxed);
            }
        }
    }

    /// Approximate queue size (may be stale under concurrent access).
    size_t size() const {
        size_t h = head_.load(std::memory_order_relaxed);
        size_t t = tail_.load(std::memory_order_relaxed);
        return h - t;
    }

    static constexpr size_t capacity() { return N; }

    bool empty() const { return size() == 0; }
};

template<typename T> using MPMCQueue1K  = MPMCQueue<T, 1024>;
template<typename T> using MPMCQueue4K  = MPMCQueue<T, 4096>;
template<typename T> using MPMCQueue16K = MPMCQueue<T, 16384>;
template<typename T> using MPMCQueue64K = MPMCQueue<T, 65536>;

}  // namespace utils
}  // namespace chronos
