#pragma once

#include "chronos/core/types.hpp"
#include "chronos/logging/log_reader.hpp"
#include <cstdint>
#include <functional>
#include <queue>
#include <vector>

namespace chronos {
namespace backtest {

// ============================================================================
// TimeReplayer — merge multiple binary log streams by timestamp order
// ============================================================================
//
// Accepts one or more LogReader instances (e.g., tick + order + fill from
// the same day, or multiple days of tick data). Uses a min-heap priority
// queue to deliver events in strict timestamp order.
//
// Typical usage:
//   TimeReplayer replayer;
//   replayer.addStream(tickReader);
//   replayer.addStream(orderReader);
//   replayer.addStream(fillReader);
//   replayer.setTickCallback([](const Tick& t) { ... });
//   replayer.setAcceleration(10.0);
//   while (replayer.advanceToNextEvent()) { ... }
//
//   // Or advance to a specific timestamp:
//   replayer.advanceTo(end_of_day_us);

class TimeReplayer {
public:
    using TickCallback  = std::function<void(const Tick&)>;
    using OrderCallback = std::function<void(const OrderRequest&)>;
    using FillCallback  = std::function<void(const Fill&)>;

    TimeReplayer() = default;
    ~TimeReplayer() = default;

    TimeReplayer(const TimeReplayer&) = delete;
    TimeReplayer& operator=(const TimeReplayer&) = delete;

    // --- Stream Management ---

    /// Add a log reader as an event source. The reader must remain valid
    /// for the lifetime of the replayer (caller owns it).
    void addStream(logging::LogReader& reader);

    /// Total number of events across all streams.
    size_t totalEvents() const { return total_events_; }

    /// Number of events delivered so far.
    size_t eventsProcessed() const { return events_processed_; }

    /// True when all streams are exhausted.
    bool isExhausted() const { return events_processed_ >= total_events_; }

    // --- Callbacks ---

    void setTickCallback(TickCallback cb)   { tick_cb_  = std::move(cb); }
    void setOrderCallback(OrderCallback cb) { order_cb_ = std::move(cb); }
    void setFillCallback(FillCallback cb)   { fill_cb_  = std::move(cb); }

    // --- Time Control ---

    /// Current virtual time (timestamp of last delivered event, or 0).
    uint64_t getCurrentTime() const { return current_time_; }

    /// Advance by one event. Returns false when no events remain.
    bool advanceToNextEvent();

    /// Advance until current time >= target_us (or stream exhausted).
    /// Returns the number of events delivered in this call.
    size_t advanceTo(uint64_t target_us);

    /// Fast-forward all remaining events.
    size_t advanceToEnd();

    /// Set replay speed multiplier (1.0 = real-time, 0 = as fast as possible).
    void setAcceleration(double factor) { acceleration_ = factor; }
    double acceleration() const { return acceleration_; }

    /// Pause / resume (paused replayer's advanceToNextEvent is no-op).
    void pause()  { paused_ = true; }
    void resume() { paused_ = false; }
    bool isPaused() const { return paused_; }

private:
    // Internal: one position within a log stream
    struct StreamPos {
        logging::LogReader* reader;
        size_t pos;
    };

    // Min-heap entry: (timestamp, stream_index)
    struct HeapEntry {
        uint64_t timestamp;
        size_t   stream_idx;

        bool operator>(const HeapEntry& o) const {
            return timestamp > o.timestamp;
        }
    };

    // Advance a stream to the next valid record (skipping invalid ones).
    // Returns true if advanced, false if stream exhausted.
    bool advanceStream(size_t stream_idx);

    // Rebuild the heap from current stream positions.
    void rebuildHeap();

    std::vector<StreamPos>              streams_;
    std::priority_queue<HeapEntry, std::vector<HeapEntry>, std::greater<>> heap_;

    TickCallback   tick_cb_;
    OrderCallback  order_cb_;
    FillCallback   fill_cb_;

    uint64_t current_time_{0};
    size_t   events_processed_{0};
    size_t   total_events_{0};
    double   acceleration_{0.0};   // 0 = as fast as possible
    bool     paused_{false};
};

}  // namespace backtest
}  // namespace chronos
