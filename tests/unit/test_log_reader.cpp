#include <gtest/gtest.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

#include "chronos/logging/log_reader.hpp"
#include "chronos/logging/log_writer.hpp"
#include "chronos/core/types.hpp"

using namespace chronos;
using namespace chronos::logging;

namespace fs = std::filesystem;

// ============================================================================
// Helpers
// ============================================================================

namespace {

const std::string TEST_LOG_DIR = "/tmp/chronos_test_logreader";

void cleanDir() {
    std::error_code ec;
    if (fs::exists(TEST_LOG_DIR, ec)) {
        fs::remove_all(TEST_LOG_DIR, ec);
    }
}

// Write a tick log file and return its path
std::string writeTickFile(const std::string& dir, int count = 10) {
    LogWriter writer;
    LogConfig cfg;
    cfg.log_dir = dir;
    cfg.buffer_size = 65536;
    cfg.flush_interval_ms = 10;
    writer.initialize(dir, cfg);

    for (int i = 0; i < count; ++i) {
        Tick t;
        t.price = toDecimal(100.0 + i);
        t.quantity = toDecimal(1.0 + i * 0.1);
        t.symbol_id = 1;
        t.side = TickSide::BID;
        t.exchange_timestamp_us = static_cast<uint64_t>(1000000 + i * 1000);
        t.receive_timestamp_us = static_cast<uint64_t>(2000000 + i * 1000);
        writer.writeTick(t);
    }
    writer.flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    writer.stop();

    // Find the tick file
    for (auto& entry : fs::directory_iterator(dir)) {
        std::string name = entry.path().filename().string();
        if (name.find("tick_") != std::string::npos) {
            return entry.path().string();
        }
    }
    return "";
}

std::string writeOrderFile(const std::string& dir, int count = 5) {
    LogWriter writer;
    LogConfig cfg;
    cfg.log_dir = dir;
    cfg.buffer_size = 65536;
    cfg.flush_interval_ms = 10;
    writer.initialize(dir, cfg);

    for (int i = 0; i < count; ++i) {
        OrderRequest o;
        o.order_id = static_cast<uint64_t>(100 + i);
        o.timestamp_us = static_cast<uint64_t>(3000000 + i * 500);
        o.price = toDecimal(200.0 + i);
        o.quantity = toDecimal(0.5 + i * 0.1);
        o.symbol_id = 1;
        o.side = OrderSide::BUY;
        o.type = OrderType::LIMIT;
        o.tif = TimeInForce::GTC;
        writer.writeOrder(o);
    }
    writer.flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    writer.stop();

    for (auto& entry : fs::directory_iterator(dir)) {
        std::string name = entry.path().filename().string();
        if (name.find("order_") != std::string::npos) {
            return entry.path().string();
        }
    }
    return "";
}

std::string writeFillFile(const std::string& dir, int count = 5) {
    LogWriter writer;
    LogConfig cfg;
    cfg.log_dir = dir;
    cfg.buffer_size = 65536;
    cfg.flush_interval_ms = 10;
    writer.initialize(dir, cfg);

    for (int i = 0; i < count; ++i) {
        Fill f;
        f.execution_id = static_cast<uint64_t>(1000 + i);
        f.order_id = static_cast<uint64_t>(100 + i);
        f.fill_price = toDecimal(150.0 + i);
        f.fill_quantity = toDecimal(0.1 + i * 0.05);
        f.symbol_id = 1;
        f.side = OrderSide::BUY;
        f.exchange_timestamp_us = static_cast<uint64_t>(5000000 + i * 2000);
        writer.writeFill(f);
    }
    writer.flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    writer.stop();

    for (auto& entry : fs::directory_iterator(dir)) {
        std::string name = entry.path().filename().string();
        if (name.find("fill_") != std::string::npos) {
            return entry.path().string();
        }
    }
    return "";
}

}  // namespace

// ============================================================================
// 1. Open & Lifecycle
// ============================================================================

TEST(LogReaderTest, OpenValidFile) {
    cleanDir();
    fs::create_directories(TEST_LOG_DIR);

    std::string path = writeTickFile(TEST_LOG_DIR, 5);
    ASSERT_FALSE(path.empty());

    LogReader reader;
    EXPECT_TRUE(reader.open(path));
    EXPECT_TRUE(reader.isOpen());
    EXPECT_EQ(reader.logType(), 0u);
    EXPECT_EQ(reader.recordCount(), 5u);
    EXPECT_EQ(reader.recordSize(), sizeof(Tick));

    reader.close();
    EXPECT_FALSE(reader.isOpen());
    cleanDir();
}

TEST(LogReaderTest, OpenNonExistentFile) {
    LogReader reader;
    EXPECT_FALSE(reader.open("/tmp/chronos_nonexistent_file_12345.bin"));
    EXPECT_FALSE(reader.isOpen());
}

TEST(LogReaderTest, OpenEmptyFile) {
    cleanDir();
    fs::create_directories(TEST_LOG_DIR);
    std::string path = TEST_LOG_DIR + "/empty.bin";
    {
        std::ofstream f(path, std::ios::binary);
        // Write nothing — 0 bytes
    }

    LogReader reader;
    EXPECT_FALSE(reader.open(path));
    EXPECT_FALSE(reader.isOpen());
    cleanDir();
}

TEST(LogReaderTest, OpenFileTooSmall) {
    cleanDir();
    fs::create_directories(TEST_LOG_DIR);
    std::string path = TEST_LOG_DIR + "/small.bin";
    {
        std::ofstream f(path, std::ios::binary);
        char c = 'x';
        f.write(&c, 1);  // 1 byte — too small for header
    }

    LogReader reader;
    EXPECT_FALSE(reader.open(path));
    cleanDir();
}

TEST(LogReaderTest, OpenCorruptedMagic) {
    cleanDir();
    fs::create_directories(TEST_LOG_DIR);
    std::string path = TEST_LOG_DIR + "/bad_magic.bin";
    {
        std::ofstream f(path, std::ios::binary);
        // Write HEADER_SIZE bytes of zeros (invalid magic)
        char zeros[64] = {};
        f.write(zeros, 64);
    }

    LogReader reader;
    EXPECT_FALSE(reader.open(path));
    cleanDir();
}

TEST(LogReaderTest, DoubleCloseIsSafe) {
    cleanDir();
    fs::create_directories(TEST_LOG_DIR);

    std::string path = writeTickFile(TEST_LOG_DIR, 3);
    ASSERT_FALSE(path.empty());

    LogReader reader;
    reader.open(path);
    reader.close();
    reader.close();  // no-op
    EXPECT_FALSE(reader.isOpen());
    cleanDir();
}

TEST(LogReaderTest, MoveConstructor) {
    cleanDir();
    fs::create_directories(TEST_LOG_DIR);

    std::string path = writeTickFile(TEST_LOG_DIR, 4);
    ASSERT_FALSE(path.empty());

    LogReader r1;
    ASSERT_TRUE(r1.open(path));
    EXPECT_EQ(r1.recordCount(), 4u);

    LogReader r2(std::move(r1));
    EXPECT_FALSE(r1.isOpen());       // r1 emptied
    EXPECT_TRUE(r2.isOpen());        // r2 owns the mapping
    EXPECT_EQ(r2.recordCount(), 4u);

    r2.close();
    cleanDir();
}

TEST(LogReaderTest, MoveAssignment) {
    cleanDir();
    fs::create_directories(TEST_LOG_DIR);

    std::string path = writeTickFile(TEST_LOG_DIR, 2);
    ASSERT_FALSE(path.empty());

    LogReader r1;
    r1.open(path);
    LogReader r2;
    r2 = std::move(r1);

    EXPECT_FALSE(r1.isOpen());
    EXPECT_TRUE(r2.isOpen());
    EXPECT_EQ(r2.recordCount(), 2u);

    r2.close();
    cleanDir();
}

// ============================================================================
// 2. Header Parsing
// ============================================================================

TEST(LogReaderTest, HeaderMagic) {
    cleanDir();
    fs::create_directories(TEST_LOG_DIR);

    std::string path = writeTickFile(TEST_LOG_DIR, 1);
    ASSERT_FALSE(path.empty());

    LogReader reader;
    ASSERT_TRUE(reader.open(path));
    EXPECT_EQ(reader.header().magic, 0x01304E4F524843ULL);
    EXPECT_EQ(reader.header().version, 1u);
    reader.close();
    cleanDir();
}

TEST(LogReaderTest, HeaderLogType) {
    cleanDir();
    fs::create_directories(TEST_LOG_DIR);

    {
        std::string path = writeTickFile(TEST_LOG_DIR, 1);
        LogReader r;
        r.open(path);
        EXPECT_EQ(r.logType(), 0u);
        r.close();
    }
    {
        std::string path = writeOrderFile(TEST_LOG_DIR, 1);
        LogReader r;
        r.open(path);
        EXPECT_EQ(r.logType(), 1u);
        r.close();
    }
    {
        std::string path = writeFillFile(TEST_LOG_DIR, 1);
        LogReader r;
        r.open(path);
        EXPECT_EQ(r.logType(), 2u);
        r.close();
    }
    cleanDir();
}

TEST(LogReaderTest, HeaderCreatedTimestamp) {
    cleanDir();
    fs::create_directories(TEST_LOG_DIR);

    std::string path = writeTickFile(TEST_LOG_DIR, 1);
    LogReader reader;
    ASSERT_TRUE(reader.open(path));
    // Timestamp should be recent (after 2020)
    EXPECT_GT(reader.createdTimestamp(), 1577836800000000ULL);
    reader.close();
    cleanDir();
}

// ============================================================================
// 3. Record Access
// ============================================================================

TEST(LogReaderTest, ReadTickRecords) {
    cleanDir();
    fs::create_directories(TEST_LOG_DIR);

    std::string path = writeTickFile(TEST_LOG_DIR, 10);
    ASSERT_FALSE(path.empty());

    LogReader reader;
    ASSERT_TRUE(reader.open(path));
    ASSERT_EQ(reader.recordCount(), 10u);

    // Check first tick
    const Tick* t0 = reader.tickAt(0);
    ASSERT_NE(t0, nullptr);
    EXPECT_NEAR(toDouble(t0->price), 100.0, 1e-9);
    EXPECT_NEAR(toDouble(t0->quantity), 1.0, 1e-9);
    EXPECT_EQ(t0->exchange_timestamp_us, 1000000ULL);

    // Check last tick
    const Tick* t9 = reader.tickAt(9);
    ASSERT_NE(t9, nullptr);
    EXPECT_NEAR(toDouble(t9->price), 109.0, 1e-9);

    // Out of bounds
    EXPECT_EQ(reader.tickAt(10), nullptr);
    reader.close();
    cleanDir();
}

TEST(LogReaderTest, ReadOrderRecords) {
    cleanDir();
    fs::create_directories(TEST_LOG_DIR);

    std::string path = writeOrderFile(TEST_LOG_DIR, 5);
    ASSERT_FALSE(path.empty());

    LogReader reader;
    ASSERT_TRUE(reader.open(path));
    ASSERT_EQ(reader.recordCount(), 5u);

    const OrderRequest* o0 = reader.orderAt(0);
    ASSERT_NE(o0, nullptr);
    EXPECT_EQ(o0->order_id, 100ULL);
    EXPECT_NEAR(toDouble(o0->price), 200.0, 1e-9);

    // Wrong type accessor
    EXPECT_EQ(reader.tickAt(0), nullptr);
    EXPECT_EQ(reader.fillAt(0), nullptr);

    reader.close();
    cleanDir();
}

TEST(LogReaderTest, ReadFillRecords) {
    cleanDir();
    fs::create_directories(TEST_LOG_DIR);

    std::string path = writeFillFile(TEST_LOG_DIR, 5);
    ASSERT_FALSE(path.empty());

    LogReader reader;
    ASSERT_TRUE(reader.open(path));
    ASSERT_EQ(reader.recordCount(), 5u);

    const Fill* f0 = reader.fillAt(0);
    ASSERT_NE(f0, nullptr);
    EXPECT_EQ(f0->execution_id, 1000ULL);
    EXPECT_EQ(f0->order_id, 100ULL);
    EXPECT_NEAR(toDouble(f0->fill_price), 150.0, 1e-9);

    reader.close();
    cleanDir();
}

TEST(LogReaderTest, RecordAtRaw) {
    cleanDir();
    fs::create_directories(TEST_LOG_DIR);

    std::string path = writeTickFile(TEST_LOG_DIR, 3);
    LogReader reader;
    ASSERT_TRUE(reader.open(path));

    const void* r0 = reader.recordAt(0);
    const void* r1 = reader.recordAt(1);
    ASSERT_NE(r0, nullptr);
    ASSERT_NE(r1, nullptr);

    // Records should be recordSize apart in memory
    auto diff = static_cast<const uint8_t*>(r1) - static_cast<const uint8_t*>(r0);
    EXPECT_EQ(static_cast<size_t>(diff), reader.recordSize());

    // Out of bounds
    EXPECT_EQ(reader.recordAt(3), nullptr);

    reader.close();
    cleanDir();
}

// ============================================================================
// 4. Timestamp Seeking
// ============================================================================

TEST(LogReaderTest, TimestampAtTick) {
    cleanDir();
    fs::create_directories(TEST_LOG_DIR);

    std::string path = writeTickFile(TEST_LOG_DIR, 10);
    LogReader reader;
    ASSERT_TRUE(reader.open(path));

    // First tick has exchange_timestamp_us = 1000000
    EXPECT_EQ(reader.timestampAt(0), 1000000ULL);
    // Last tick: 1000000 + 9 * 1000 = 1009000
    EXPECT_EQ(reader.timestampAt(9), 1009000ULL);

    reader.close();
    cleanDir();
}

TEST(LogReaderTest, TimestampAtOrder) {
    cleanDir();
    fs::create_directories(TEST_LOG_DIR);

    std::string path = writeOrderFile(TEST_LOG_DIR, 5);
    LogReader reader;
    ASSERT_TRUE(reader.open(path));

    // First order timestamp_us = 3000000
    EXPECT_EQ(reader.timestampAt(0), 3000000ULL);

    reader.close();
    cleanDir();
}

TEST(LogReaderTest, TimestampAtFill) {
    cleanDir();
    fs::create_directories(TEST_LOG_DIR);

    std::string path = writeFillFile(TEST_LOG_DIR, 5);
    LogReader reader;
    ASSERT_TRUE(reader.open(path));

    // First fill exchange_timestamp_us = 5000000
    EXPECT_EQ(reader.timestampAt(0), 5000000ULL);

    reader.close();
    cleanDir();
}

TEST(LogReaderTest, SeekToTimestampExact) {
    cleanDir();
    fs::create_directories(TEST_LOG_DIR);

    std::string path = writeTickFile(TEST_LOG_DIR, 10);
    LogReader reader;
    ASSERT_TRUE(reader.open(path));

    // Exact match: 1004000 is the 5th tick (index 4)
    size_t idx = reader.seekToTimestamp(1004000);
    EXPECT_EQ(idx, 4u);
    EXPECT_EQ(reader.timestampAt(idx), 1004000ULL);

    reader.close();
    cleanDir();
}

TEST(LogReaderTest, SeekToTimestampBetween) {
    cleanDir();
    fs::create_directories(TEST_LOG_DIR);

    std::string path = writeTickFile(TEST_LOG_DIR, 10);
    LogReader reader;
    ASSERT_TRUE(reader.open(path));

    // 1004500 is between ticks 4 (1004000) and 5 (1005000)
    // Should return index 5 (first with ts >= 1004500)
    size_t idx = reader.seekToTimestamp(1004500);
    EXPECT_EQ(idx, 5u);
    EXPECT_GE(reader.timestampAt(idx), 1004500ULL);

    reader.close();
    cleanDir();
}

TEST(LogReaderTest, SeekBeforeFirst) {
    cleanDir();
    fs::create_directories(TEST_LOG_DIR);

    std::string path = writeTickFile(TEST_LOG_DIR, 10);
    LogReader reader;
    ASSERT_TRUE(reader.open(path));

    size_t idx = reader.seekToTimestamp(0);
    EXPECT_EQ(idx, 0u);

    reader.close();
    cleanDir();
}

TEST(LogReaderTest, SeekAfterLast) {
    cleanDir();
    fs::create_directories(TEST_LOG_DIR);

    std::string path = writeTickFile(TEST_LOG_DIR, 10);
    LogReader reader;
    ASSERT_TRUE(reader.open(path));

    // Timestamp larger than last record
    size_t idx = reader.seekToTimestamp(9999999);
    EXPECT_EQ(idx, reader.recordCount());

    reader.close();
    cleanDir();
}

TEST(LogReaderTest, SeekEmpty) {
    // Create a file with header only, no records
    cleanDir();
    fs::create_directories(TEST_LOG_DIR);

    // Write just the header manually
    std::string path = TEST_LOG_DIR + "/empty_records.bin";
    {
        std::ofstream f(path, std::ios::binary);
        uint64_t magic = 0x01304E4F524843ULL;
        uint32_t version = 1;
        uint32_t log_type = 0;
        uint64_t ts = 1000000;
        f.write(reinterpret_cast<const char*>(&magic), 8);
        f.write(reinterpret_cast<const char*>(&version), 4);
        f.write(reinterpret_cast<const char*>(&log_type), 4);
        f.write(reinterpret_cast<const char*>(&ts), 8);
        // Pad to HEADER_SIZE=64
        char padding[40] = {};
        f.write(padding, 40);
    }

    LogReader reader;
    ASSERT_TRUE(reader.open(path));
    EXPECT_EQ(reader.recordCount(), 0u);
    EXPECT_EQ(reader.seekToTimestamp(0), 0u);

    reader.close();
    cleanDir();
}

// ============================================================================
// 5. Corruption Handling
// ============================================================================

TEST(LogReaderTest, TrailingPartialRecordIgnored) {
    cleanDir();
    fs::create_directories(TEST_LOG_DIR);

    // Write a valid header + 1 tick + extra trailing bytes
    std::string path = TEST_LOG_DIR + "/partial.bin";
    {
        std::ofstream f(path, std::ios::binary);
        // Header
        uint64_t magic = 0x01304E4F524843ULL;
        uint32_t version = 1;
        uint32_t log_type = 0;
        uint64_t ts = 1000000;
        f.write(reinterpret_cast<const char*>(&magic), 8);
        f.write(reinterpret_cast<const char*>(&version), 4);
        f.write(reinterpret_cast<const char*>(&log_type), 4);
        f.write(reinterpret_cast<const char*>(&ts), 8);
        // Pad to HEADER_SIZE=64
        char padding[40] = {};
        f.write(padding, 40);
        // One valid tick (64 bytes)
        Tick t;
        t.price = toDecimal(50.0);
        t.quantity = toDecimal(1.0);
        t.exchange_timestamp_us = 1000;
        f.write(reinterpret_cast<const char*>(&t), sizeof(Tick));
        // Trailing garbage (30 bytes — not a full record)
        char garbage[30] = {};
        f.write(garbage, 30);
    }

    LogReader reader;
    ASSERT_TRUE(reader.open(path));
    // Should only count 1 full record
    EXPECT_EQ(reader.recordCount(), 1u);
    EXPECT_NE(reader.tickAt(0), nullptr);
    EXPECT_EQ(reader.tickAt(1), nullptr);

    reader.close();
    cleanDir();
}

TEST(LogReaderTest, ValidateRecordPasses) {
    cleanDir();
    fs::create_directories(TEST_LOG_DIR);

    std::string path = writeTickFile(TEST_LOG_DIR, 1);
    LogReader reader;
    ASSERT_TRUE(reader.open(path));

    std::string error;
    EXPECT_TRUE(reader.validateRecord(0, error));
    EXPECT_TRUE(error.empty());

    reader.close();
    cleanDir();
}

TEST(LogReaderTest, ValidateRecordOutOfBounds) {
    cleanDir();
    fs::create_directories(TEST_LOG_DIR);

    std::string path = writeTickFile(TEST_LOG_DIR, 1);
    LogReader reader;
    ASSERT_TRUE(reader.open(path));

    std::string error;
    EXPECT_FALSE(reader.validateRecord(999, error));
    EXPECT_FALSE(error.empty());

    reader.close();
    cleanDir();
}

TEST(LogReaderTest, ValidateOrderRecord) {
    cleanDir();
    fs::create_directories(TEST_LOG_DIR);

    std::string path = writeOrderFile(TEST_LOG_DIR, 1);
    LogReader reader;
    ASSERT_TRUE(reader.open(path));

    std::string error;
    EXPECT_TRUE(reader.validateRecord(0, error));

    reader.close();
    cleanDir();
}

// ============================================================================
// 6. LogFileSet
// ============================================================================

TEST(LogReaderTest, OpenLogDirectoryWritesThenReads) {
    cleanDir();
    fs::create_directories(TEST_LOG_DIR);

    // Write all three log types
    writeTickFile(TEST_LOG_DIR, 3);
    writeOrderFile(TEST_LOG_DIR, 4);
    writeFillFile(TEST_LOG_DIR, 5);

    // Open via LogFileSet
    auto set = openLogDirectory(TEST_LOG_DIR);
    EXPECT_TRUE(set.tick.isOpen());
    EXPECT_TRUE(set.order.isOpen());
    EXPECT_TRUE(set.fill.isOpen());

    EXPECT_EQ(set.tick.recordCount(), 3u);
    EXPECT_EQ(set.order.recordCount(), 4u);
    EXPECT_EQ(set.fill.recordCount(), 5u);

    EXPECT_EQ(set.tick.logType(), 0u);
    EXPECT_EQ(set.order.logType(), 1u);
    EXPECT_EQ(set.fill.logType(), 2u);

    cleanDir();
}

TEST(LogReaderTest, OpenLogDirectoryEmptyDir) {
    cleanDir();
    fs::create_directories(TEST_LOG_DIR);

    auto set = openLogDirectory(TEST_LOG_DIR);
    EXPECT_FALSE(set.tick.isOpen());
    EXPECT_FALSE(set.order.isOpen());
    EXPECT_FALSE(set.fill.isOpen());

    cleanDir();
}

TEST(LogReaderTest, OpenLogDirectoryNonExistent) {
    auto set = openLogDirectory("/tmp/chronos_dir_does_not_exist_xyz");
    EXPECT_FALSE(set.tick.isOpen());
    EXPECT_FALSE(set.order.isOpen());
    EXPECT_FALSE(set.fill.isOpen());
}

// ============================================================================
// Global cleanup
// ============================================================================

namespace {
struct LogReaderEnv : ::testing::Environment {
    void SetUp() override   { cleanDir(); }
    void TearDown() override { cleanDir(); }
};
const auto _env_reg = [] {
    ::testing::AddGlobalTestEnvironment(new LogReaderEnv);
    return 0;
}();
}  // namespace
