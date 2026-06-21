#include "chronos/backtest/time_replayer.hpp"
#include <algorithm>

namespace chronos {
namespace backtest {

// ============================================================================
// Stream Management
// ============================================================================

void TimeReplayer::addStream(logging::LogReader& reader) {
    if (reader.recordCount() == 0) return;  // skip empty streams

    StreamPos sp{&reader, 0};
    size_t idx = streams_.size();
    streams_.push_back(sp);
    total_events_ += reader.recordCount();

    // Push first event onto heap
    uint64_t ts = reader.timestampAt(0);
    heap_.push(HeapEntry{ts, idx});
}

// ============================================================================
// Advance
// ============================================================================

bool TimeReplayer::advanceToNextEvent() {
    if (paused_) return false;
    if (isExhausted()) return false;

    // Pop next event from heap
    HeapEntry entry = heap_.top();
    heap_.pop();

    size_t idx = entry.stream_idx;
    StreamPos& sp = streams_[idx];

    // Deliver the event
    switch (sp.reader->logType()) {
        case 0: {  // TICK
            const Tick* t = sp.reader->tickAt(sp.pos);
            if (t && tick_cb_) tick_cb_(*t);
            break;
        }
        case 1: {  // ORDER
            const OrderRequest* o = sp.reader->orderAt(sp.pos);
            if (o && order_cb_) order_cb_(*o);
            break;
        }
        case 2: {  // FILL
            const Fill* f = sp.reader->fillAt(sp.pos);
            if (f && fill_cb_) fill_cb_(*f);
            break;
        }
    }

    current_time_ = entry.timestamp;
    events_processed_++;

    // Advance this stream
    sp.pos++;
    if (sp.pos < sp.reader->recordCount()) {
        uint64_t next_ts = sp.reader->timestampAt(sp.pos);
        heap_.push(HeapEntry{next_ts, idx});
    }

    return true;
}

size_t TimeReplayer::advanceTo(uint64_t target_us) {
    size_t count = 0;
    while (!isExhausted() && current_time_ < target_us) {
        if (!advanceToNextEvent()) break;
        count++;
    }
    return count;
}

size_t TimeReplayer::advanceToEnd() {
    size_t count = 0;
    while (!isExhausted()) {
        if (!advanceToNextEvent()) break;
        count++;
    }
    return count;
}

}  // namespace backtest
}  // namespace chronos
