#include <gtest/gtest.h>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

#include "chronos/logging/log_writer.hpp"
#include "chronos/core/types.hpp"

using namespace chronos;
using namespace chronos::logging;

namespace fs = std::filesystem;

// ============================================================================
// Helpers
// ============================================================================

namespace {

const std::string TEST_LOG_DIR = "/tmp/chronos_test_logs";

LogConfig makeConfig() {
    LogConfig cfg;
    cfg.log_dir = TEST_LOG_DIR;
    cfg.buffer_size = 65536;
    cfg.flush_interval_ms = 50;
    cfg.retention_days = 7;
    cfg.enable_tick_logging = true;
    cfg.enable_order_logging = true;
    cfg.enable_fill_logging = true;
    return cfg;
}

void cleanDir() {
    std::error_code ec;
    if (fs::exists(TEST_LOG_DIR, ec)) {
        fs::remove_all(TEST_LOG_DIR, ec);
    }
}

Tick makeTick(double price, double qty) {
    Tick t;
    t.price = toDecimal(price);
    t.quantity = toDecimal(qty);
    t.symbol_id = 1;
    t.side = TickSide::BID;
    t.exchange_timestamp_us = 1000000;
    t.receive_timestamp_us = 2000000;
    return t;
}

OrderRequest makeOrder(double price, double qty) {
    OrderRequest o;
    o.order_id = 1;
    o.price = toDecimal(price);
    o.quantity = toDecimal(qty);
    o.symbol_id = 1;
    o.side = OrderSide::BUY;
    o.type = OrderType::LIMIT;
    return o;
}

Fill makeFill(double price, double qty) {
    Fill f;
    f.order_id = 1;
    f.fill_price = toDecimal(price);
    f.fill_quantity = toDecimal(qty);
    f.symbol_id = 1;
    return f;
}

}  // namespace

// ============================================================================
// 1. Initialization & Lifecycle
// ============================================================================

TEST(LogWriterTest, InitializeCreatesDirectory) {
    cleanDir();

    LogWriter writer;
    EXPECT_TRUE(writer.initialize(TEST_LOG_DIR, makeConfig()));
    EXPECT_TRUE(writer.isRunning());

    std::error_code ec;
    EXPECT_TRUE(fs::exists(TEST_LOG_DIR, ec));

    writer.stop();
    cleanDir();
}

TEST(LogWriterTest, InitializeCreatesLogFiles) {
    cleanDir();

    LogWriter writer;
    ASSERT_TRUE(writer.initialize(TEST_LOG_DIR, makeConfig()));

    // Flush to ensure files are written
    writer.flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    bool found_tick = false, found_order = false, found_fill = false;
    for (auto& entry : fs::directory_iterator(TEST_LOG_DIR)) {
        std::string name = entry.path().filename().string();
        if (name.find("tick_") != std::string::npos) found_tick = true;
        if (name.find("order_") != std::string::npos) found_order = true;
        if (name.find("fill_") != std::string::npos) found_fill = true;
    }
    EXPECT_TRUE(found_tick);
    EXPECT_TRUE(found_order);
    EXPECT_TRUE(found_fill);

    writer.stop();
    cleanDir();
}

TEST(LogWriterTest, StopClosesCleanly) {
    cleanDir();

    LogWriter writer;
    ASSERT_TRUE(writer.initialize(TEST_LOG_DIR, makeConfig()));
    writer.stop();
    EXPECT_FALSE(writer.isRunning());

    cleanDir();
}

TEST(LogWriterTest, DoubleStopIsSafe) {
    cleanDir();

    LogWriter writer;
    ASSERT_TRUE(writer.initialize(TEST_LOG_DIR, makeConfig()));
    writer.stop();
    writer.stop();  // no-op
    EXPECT_FALSE(writer.isRunning());

    cleanDir();
}

// ============================================================================
// 2. Write & Verify
// ============================================================================

TEST(LogWriterTest, WriteTickIncrementsCounter) {
    cleanDir();

    LogWriter writer;
    ASSERT_TRUE(writer.initialize(TEST_LOG_DIR, makeConfig()));

    EXPECT_TRUE(writer.writeTick(makeTick(100.0, 1.0)));
    EXPECT_TRUE(writer.writeTick(makeTick(101.0, 2.0)));
    EXPECT_TRUE(writer.writeTick(makeTick(102.0, 3.0)));

    EXPECT_EQ(writer.ticksWritten(), 3u);

    writer.stop();
    cleanDir();
}

TEST(LogWriterTest, WriteOrderIncrementsCounter) {
    cleanDir();

    LogWriter writer;
    ASSERT_TRUE(writer.initialize(TEST_LOG_DIR, makeConfig()));

    EXPECT_TRUE(writer.writeOrder(makeOrder(100.0, 0.5)));
    EXPECT_TRUE(writer.writeOrder(makeOrder(200.0, 1.0)));

    EXPECT_EQ(writer.ordersWritten(), 2u);

    writer.stop();
    cleanDir();
}

TEST(LogWriterTest, WriteFillIncrementsCounter) {
    cleanDir();

    LogWriter writer;
    ASSERT_TRUE(writer.initialize(TEST_LOG_DIR, makeConfig()));

    EXPECT_TRUE(writer.writeFill(makeFill(50.0, 0.1)));
    EXPECT_TRUE(writer.writeFill(makeFill(60.0, 0.2)));
    EXPECT_TRUE(writer.writeFill(makeFill(70.0, 0.3)));

    EXPECT_EQ(writer.fillsWritten(), 3u);

    writer.stop();
    cleanDir();
}

TEST(LogWriterTest, DataWrittenToFile) {
    cleanDir();

    LogWriter writer;
    ASSERT_TRUE(writer.initialize(TEST_LOG_DIR, makeConfig()));

    // Write several ticks
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(writer.writeTick(makeTick(100.0 + i, 1.0)));
    }

    // Flush and wait
    writer.flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    writer.stop();

    // Find the tick file and verify it has data (header + records)
    for (auto& entry : fs::directory_iterator(TEST_LOG_DIR)) {
        std::string name = entry.path().filename().string();
        if (name.find("tick_") != std::string::npos) {
            auto sz = entry.file_size();
            // 24-byte header + 10 * 64-byte ticks = 664 bytes
            EXPECT_GE(sz, 24u + 10u * sizeof(Tick));
        }
    }

    cleanDir();
}

// ============================================================================
// 3. Buffer Full / Backpressure
// ============================================================================

TEST(LogWriterTest, BufferFullReturnsFalse) {
    cleanDir();

    LogConfig cfg = makeConfig();
    cfg.buffer_size = 128;  // Very small buffer
    cfg.flush_interval_ms = 1000;  // Don't flush automatically

    LogWriter writer;
    ASSERT_TRUE(writer.initialize(TEST_LOG_DIR, cfg));

    // Write ticks until buffer is full
    int written = 0;
    while (writer.writeTick(makeTick(100.0, 1.0))) {
        written++;
    }

    // Should have written at least 1, but less than buffer capacity
    EXPECT_GT(written, 0);
    EXPECT_LT(written, 100);

    // All further writes fail
    EXPECT_FALSE(writer.writeTick(makeTick(100.0, 1.0)));

    // Flush clears buffer
    writer.flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Now writes should succeed again
    EXPECT_TRUE(writer.writeTick(makeTick(100.0, 1.0)));

    writer.stop();
    cleanDir();
}

// ============================================================================
// 4. Rotation
// ============================================================================

TEST(LogWriterTest, RotateNowClosesAndReopens) {
    cleanDir();

    LogWriter writer;
    ASSERT_TRUE(writer.initialize(TEST_LOG_DIR, makeConfig()));

    EXPECT_TRUE(writer.writeTick(makeTick(100.0, 1.0)));
    writer.flush();

    // Rotation should succeed without crash
    writer.rotateNow();
    EXPECT_TRUE(writer.writeTick(makeTick(200.0, 1.0)));

    writer.stop();
    cleanDir();
}

// ============================================================================
// 5. Concurrent Writes
// ============================================================================

TEST(LogWriterTest, ConcurrentWrites) {
    cleanDir();

    LogConfig cfg = makeConfig();
    cfg.buffer_size = 1024 * 1024;  // 1MB — large enough not to overflow

    LogWriter writer;
    ASSERT_TRUE(writer.initialize(TEST_LOG_DIR, cfg));

    constexpr int N = 200;
    std::thread t1([&] {
        for (int i = 0; i < N; ++i) {
            writer.writeTick(makeTick(100.0 + i * 0.1, 0.1));
        }
    });
    std::thread t2([&] {
        for (int i = 0; i < N; ++i) {
            writer.writeTick(makeTick(200.0 + i * 0.1, 0.2));
        }
    });

    t1.join();
    t2.join();

    EXPECT_EQ(writer.ticksWritten(), static_cast<uint64_t>(N * 2));

    writer.stop();
    cleanDir();
}

// ============================================================================
// Global cleanup (runs before/after all tests in this file)
// ============================================================================

namespace {
struct LogWriterEnv : ::testing::Environment {
    void SetUp() override   { cleanDir(); }
    void TearDown() override { cleanDir(); }
};
const auto _env_reg = [] {
    ::testing::AddGlobalTestEnvironment(new LogWriterEnv);
    return 0;
}();
}  // namespace
