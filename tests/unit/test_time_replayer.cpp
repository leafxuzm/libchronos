#include <gtest/gtest.h>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

#include "chronos/backtest/time_replayer.hpp"
#include "chronos/logging/log_writer.hpp"
#include "chronos/logging/log_reader.hpp"
#include "chronos/core/types.hpp"

using namespace chronos;
using namespace chronos::backtest;
using namespace chronos::logging;

namespace fs = std::filesystem;

// ============================================================================
// Helpers
// ============================================================================

namespace {

const std::string TEST_DIR = "/tmp/chronos_test_replayer";

void cleanDir() {
    std::error_code ec;
    if (fs::exists(TEST_DIR, ec)) fs::remove_all(TEST_DIR, ec);
}

// Write tick + order + fill logs with interleaved timestamps
struct LogSet {
    std::string tick_path;
    std::string order_path;
    std::string fill_path;
};

LogSet writeLogs() {
    fs::create_directories(TEST_DIR);
    LogSet set;

    // Ticks: timestamps 1000, 3000, 5000, 7000, 9000
    {
        LogWriter w;
        LogConfig cfg;
        cfg.log_dir = TEST_DIR;
        cfg.buffer_size = 65536;
        cfg.flush_interval_ms = 10;
        w.initialize(TEST_DIR, cfg);

        for (int i = 0; i < 5; ++i) {
            Tick t;
            t.exchange_timestamp_us = static_cast<uint64_t>(1000 + i * 2000);
            t.price = toDecimal(50.0 + i);
            t.quantity = toDecimal(1.0);
            t.symbol_id = 1;
            t.side = TickSide::BID;
            w.writeTick(t);
        }
        w.flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        w.stop();
    }
    for (auto& e : fs::directory_iterator(TEST_DIR)) {
        if (e.path().filename().string().find("tick_") != std::string::npos)
            set.tick_path = e.path().string();
    }

    // Orders: timestamps 2000, 6000
    {
        LogWriter w;
        LogConfig cfg;
        cfg.log_dir = TEST_DIR;
        cfg.buffer_size = 65536;
        cfg.flush_interval_ms = 10;
        w.initialize(TEST_DIR, cfg);

        for (int i = 0; i < 2; ++i) {
            OrderRequest o;
            o.order_id = static_cast<uint64_t>(100 + i);
            o.timestamp_us = static_cast<uint64_t>(2000 + i * 4000);
            o.price = toDecimal(100.0);
            o.quantity = toDecimal(0.5);
            o.symbol_id = 1;
            o.side = OrderSide::BUY;
            o.type = OrderType::LIMIT;
            o.tif = TimeInForce::GTC;
            w.writeOrder(o);
        }
        w.flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        w.stop();
    }
    for (auto& e : fs::directory_iterator(TEST_DIR)) {
        if (e.path().filename().string().find("order_") != std::string::npos)
            set.order_path = e.path().string();
    }

    // Fills: timestamps 4000, 8000
    {
        LogWriter w;
        LogConfig cfg;
        cfg.log_dir = TEST_DIR;
        cfg.buffer_size = 65536;
        cfg.flush_interval_ms = 10;
        w.initialize(TEST_DIR, cfg);

        for (int i = 0; i < 2; ++i) {
            Fill f;
            f.order_id = static_cast<uint64_t>(100 + i);
            f.execution_id = static_cast<uint64_t>(1000 + i);
            f.exchange_timestamp_us = static_cast<uint64_t>(4000 + i * 4000);
            f.fill_price = toDecimal(55.0);
            f.fill_quantity = toDecimal(0.1);
            f.symbol_id = 1;
            f.side = OrderSide::BUY;
            w.writeFill(f);
        }
        w.flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        w.stop();
    }
    for (auto& e : fs::directory_iterator(TEST_DIR)) {
        if (e.path().filename().string().find("fill_") != std::string::npos)
            set.fill_path = e.path().string();
    }

    return set;
}

}  // namespace

// ============================================================================
// 1. Stream Management
// ============================================================================

TEST(TimeReplayerTest, EmptyReplayerIsExhausted) {
    TimeReplayer r;
    EXPECT_TRUE(r.isExhausted());
    EXPECT_EQ(r.totalEvents(), 0u);
    EXPECT_EQ(r.getCurrentTime(), 0u);
}

TEST(TimeReplayerTest, AddStreamCountsEvents) {
    cleanDir();
    LogSet logs = writeLogs();
    ASSERT_FALSE(logs.tick_path.empty());

    LogReader tick_r;
    ASSERT_TRUE(tick_r.open(logs.tick_path));

    TimeReplayer r;
    r.addStream(tick_r);
    EXPECT_EQ(r.totalEvents(), 5u);  // 5 ticks
    EXPECT_FALSE(r.isExhausted());
    cleanDir();
}

TEST(TimeReplayerTest, AddEmptyStreamIsNoOp) {
    cleanDir();
    fs::create_directories(TEST_DIR);

    // Create a log file with 0 records
    LogWriter w;
    LogConfig cfg;
    cfg.log_dir = TEST_DIR;
    cfg.buffer_size = 65536;
    cfg.flush_interval_ms = 10;
    w.initialize(TEST_DIR, cfg);
    w.flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    w.stop();

    std::string path;
    for (auto& e : fs::directory_iterator(TEST_DIR)) {
        if (e.path().filename().string().find("tick_") != std::string::npos)
            path = e.path().string();
    }
    ASSERT_FALSE(path.empty());

    LogReader reader;
    ASSERT_TRUE(reader.open(path));
    ASSERT_EQ(reader.recordCount(), 0u);

    TimeReplayer r;
    r.addStream(reader);
    EXPECT_EQ(r.totalEvents(), 0u);
    EXPECT_TRUE(r.isExhausted());
    cleanDir();
}

// ============================================================================
// 2. Event Delivery — Timestamp Order
// ============================================================================

TEST(TimeReplayerTest, DeliversInTimestampOrder) {
    cleanDir();
    LogSet logs = writeLogs();

    LogReader tick_r, order_r, fill_r;
    ASSERT_TRUE(tick_r.open(logs.tick_path));
    ASSERT_TRUE(order_r.open(logs.order_path));
    ASSERT_TRUE(fill_r.open(logs.fill_path));

    TimeReplayer r;
    r.addStream(tick_r);
    r.addStream(order_r);
    r.addStream(fill_r);

    EXPECT_EQ(r.totalEvents(), 9u);  // 5 + 2 + 2

    // Expected order: T1000, O2000, T3000, F4000, T5000, O6000, T7000, F8000, T9000
    struct Event { uint64_t ts; int type; };
    std::vector<Event> events;

    r.setTickCallback([&](const Tick& t) {
        events.push_back({t.exchange_timestamp_us, 0});
    });
    r.setOrderCallback([&](const OrderRequest& o) {
        events.push_back({o.timestamp_us, 1});
    });
    r.setFillCallback([&](const Fill& f) {
        events.push_back({f.exchange_timestamp_us, 2});
    });

    r.advanceToEnd();

    ASSERT_EQ(events.size(), 9u);
    EXPECT_EQ(events[0].ts, 1000u); EXPECT_EQ(events[0].type, 0);  // TICK
    EXPECT_EQ(events[1].ts, 2000u); EXPECT_EQ(events[1].type, 1);  // ORDER
    EXPECT_EQ(events[2].ts, 3000u); EXPECT_EQ(events[2].type, 0);  // TICK
    EXPECT_EQ(events[3].ts, 4000u); EXPECT_EQ(events[3].type, 2);  // FILL
    EXPECT_EQ(events[4].ts, 5000u); EXPECT_EQ(events[4].type, 0);  // TICK
    EXPECT_EQ(events[5].ts, 6000u); EXPECT_EQ(events[5].type, 1);  // ORDER
    EXPECT_EQ(events[6].ts, 7000u); EXPECT_EQ(events[6].type, 0);  // TICK
    EXPECT_EQ(events[7].ts, 8000u); EXPECT_EQ(events[7].type, 2);  // FILL
    EXPECT_EQ(events[8].ts, 9000u); EXPECT_EQ(events[8].type, 0);  // TICK

    EXPECT_EQ(r.eventsProcessed(), 9u);
    EXPECT_TRUE(r.isExhausted());
    cleanDir();
}

// ============================================================================
// 3. Time Control
// ============================================================================

TEST(TimeReplayerTest, AdvanceToNextEventStepsTime) {
    cleanDir();
    LogSet logs = writeLogs();

    LogReader tick_r;
    ASSERT_TRUE(tick_r.open(logs.tick_path));

    TimeReplayer r;
    r.addStream(tick_r);

    r.advanceToNextEvent();
    EXPECT_EQ(r.eventsProcessed(), 1u);
    EXPECT_EQ(r.getCurrentTime(), 1000u);

    r.advanceToNextEvent();
    EXPECT_EQ(r.eventsProcessed(), 2u);
    EXPECT_EQ(r.getCurrentTime(), 3000u);

    cleanDir();
}

TEST(TimeReplayerTest, AdvanceToTarget) {
    cleanDir();
    LogSet logs = writeLogs();

    LogReader tick_r;
    ASSERT_TRUE(tick_r.open(logs.tick_path));

    TimeReplayer r;
    r.addStream(tick_r);

    // Advance through the 3rd tick (inclusive)
    size_t n = r.advanceTo(5000);
    EXPECT_EQ(n, 3u);
    EXPECT_EQ(r.getCurrentTime(), 5000u);
    EXPECT_FALSE(r.isExhausted());

    cleanDir();
}

TEST(TimeReplayerTest, AdvanceToEnd) {
    cleanDir();
    LogSet logs = writeLogs();

    LogReader tick_r;
    ASSERT_TRUE(tick_r.open(logs.tick_path));

    TimeReplayer r;
    r.addStream(tick_r);

    size_t n = r.advanceToEnd();
    EXPECT_EQ(n, 5u);
    EXPECT_TRUE(r.isExhausted());
    EXPECT_EQ(r.getCurrentTime(), 9000u);

    cleanDir();
}

TEST(TimeReplayerTest, AdvanceToNextEventWhenExhaustedReturnsFalse) {
    TimeReplayer r;
    EXPECT_FALSE(r.advanceToNextEvent());
}

// ============================================================================
// 4. Pause / Resume
// ============================================================================

TEST(TimeReplayerTest, PausePreventsAdvance) {
    cleanDir();
    LogSet logs = writeLogs();

    LogReader tick_r;
    ASSERT_TRUE(tick_r.open(logs.tick_path));

    TimeReplayer r;
    r.addStream(tick_r);

    r.pause();
    EXPECT_TRUE(r.isPaused());

    EXPECT_FALSE(r.advanceToNextEvent());
    EXPECT_EQ(r.eventsProcessed(), 0u);

    r.resume();
    EXPECT_FALSE(r.isPaused());
    EXPECT_TRUE(r.advanceToNextEvent());
    EXPECT_EQ(r.eventsProcessed(), 1u);

    cleanDir();
}

// ============================================================================
// 5. Acceleration
// ============================================================================

TEST(TimeReplayerTest, DefaultAccelerationIsZero) {
    TimeReplayer r;
    EXPECT_EQ(r.acceleration(), 0.0);  // 0 = as fast as possible
}

TEST(TimeReplayerTest, SetAcceleration) {
    TimeReplayer r;
    r.setAcceleration(5.0);
    EXPECT_EQ(r.acceleration(), 5.0);
}

// ============================================================================
// 6. Null Callbacks Are Safe
// ============================================================================

TEST(TimeReplayerTest, NullCallbacksAreSafe) {
    cleanDir();
    LogSet logs = writeLogs();

    LogReader tick_r, order_r, fill_r;
    ASSERT_TRUE(tick_r.open(logs.tick_path));
    ASSERT_TRUE(order_r.open(logs.order_path));
    ASSERT_TRUE(fill_r.open(logs.fill_path));

    TimeReplayer r;
    r.addStream(tick_r);
    r.addStream(order_r);
    r.addStream(fill_r);

    // No callbacks set — should not crash
    size_t n = r.advanceToEnd();
    EXPECT_EQ(n, 9u);
    cleanDir();
}

// ============================================================================
// Global cleanup
// ============================================================================

namespace {
struct TimeReplayerEnv : ::testing::Environment {
    void SetUp() override   { cleanDir(); }
    void TearDown() override { cleanDir(); }
};
const auto _env_reg = [] {
    ::testing::AddGlobalTestEnvironment(new TimeReplayerEnv);
    return 0;
}();
}  // namespace
