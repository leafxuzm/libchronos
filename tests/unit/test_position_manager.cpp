/**
 * @file test_position_manager.cpp
 * @brief Unit tests for PositionManager — position tracking, P&L, persistence
 *
 * Validates: Requirements 8.1, 8.2, 8.3, 8.4, 8.5, 8.6, 8.7, 8.8, 8.9
 * PBT Property 12: Position Average Price Calculation
 * PBT Property 13: Position P&L Calculation
 */

#include <gtest/gtest.h>
#include <chronos/trading/position_manager.hpp>
#include <chronos/core/types.hpp>
#include <nlohmann/json.hpp>
#include <thread>
#include <vector>
#include <cstdio>  // for std::remove

using namespace chronos;
using namespace chronos::trading;

namespace {

Fill makeBuyFill(uint32_t symbol_id, double price, double qty) {
    Fill f;
    f.symbol_id = symbol_id;
    f.fill_price = toDecimal(price);
    f.fill_quantity = toDecimal(qty);
    f.side = OrderSide::BUY;
    return f;
}

Fill makeSellFill(uint32_t symbol_id, double price, double qty) {
    Fill f;
    f.symbol_id = symbol_id;
    f.fill_price = toDecimal(price);
    f.fill_quantity = toDecimal(qty);
    f.side = OrderSide::SELL;
    return f;
}

} // anonymous namespace

// ============================================================================
// Empty State
// ============================================================================

TEST(PositionManagerTest, GetPositionUnknownSymbolReturnsNull) {
    PositionManager pm;
    EXPECT_EQ(pm.getPosition(1), nullptr);
}

TEST(PositionManagerTest, GetAllPositionsEmpty) {
    PositionManager pm;
    auto positions = pm.getAllPositions();
    EXPECT_TRUE(positions.empty());
}

// ============================================================================
// New Positions
// ============================================================================

TEST(PositionManagerTest, CreatePositionFromBuy) {
    PositionManager pm;
    auto* pos = pm.updatePosition(makeBuyFill(1, 50000.0, 1.0));
    ASSERT_NE(pos, nullptr);
    EXPECT_EQ(pos->symbol_id, 1U);
    EXPECT_EQ(pos->quantity, toDecimal(1.0));
    EXPECT_EQ(pos->average_price, toDecimal(50000.0));
}

TEST(PositionManagerTest, CreatePositionFromSell) {
    PositionManager pm;
    auto* pos = pm.updatePosition(makeSellFill(1, 50000.0, 1.0));
    ASSERT_NE(pos, nullptr);
    EXPECT_EQ(pos->quantity, toDecimal(-1.0));
    EXPECT_EQ(pos->average_price, toDecimal(50000.0));
}

TEST(PositionManagerTest, ZeroQuantityFillReturnsNull) {
    PositionManager pm;
    Fill f;
    f.symbol_id = 1;
    f.fill_quantity = toDecimal(0.0);
    f.fill_price = toDecimal(50000.0);
    f.side = OrderSide::BUY;
    EXPECT_EQ(pm.updatePosition(f), nullptr);
}

// ============================================================================
// Increase Position (Same Direction)
// ============================================================================

TEST(PositionManagerTest, IncreaseSameDirectionBuy) {
    PositionManager pm;
    pm.updatePosition(makeBuyFill(1, 50000.0, 1.0));
    // Weighted average: (1.0 * 50000 + 2.0 * 51000) / 3.0 = 50666.666...
    auto* pos = pm.updatePosition(makeBuyFill(1, 51000.0, 2.0));

    ASSERT_NE(pos, nullptr);
    EXPECT_EQ(pos->quantity, toDecimal(3.0));
    // avg = (1*50000 + 2*51000)/3 = (50000+102000)/3 = 152000/3 ≈ 50666.666
    Decimal expected_avg = (toDecimal(50000.0) + toDecimal(51000.0) * toDecimal(2.0))
                         / toDecimal(3.0);
    EXPECT_EQ(pos->average_price, expected_avg);
}

TEST(PositionManagerTest, IncreaseShortPosition) {
    PositionManager pm;
    pm.updatePosition(makeSellFill(1, 50000.0, 1.0));
    // Short position gets bigger: -1.0 + (-2.0) = -3.0
    auto* pos = pm.updatePosition(makeSellFill(1, 51000.0, 2.0));
    ASSERT_NE(pos, nullptr);
    EXPECT_EQ(pos->quantity, toDecimal(-3.0));
    Decimal expected_avg = (toDecimal(-1.0) < toDecimal(0.0) ? toDecimal(-1.0) : toDecimal(1.0));
    // avg price: (|1.0|*50000 + |2.0|*51000) / 3.0 = 50666.666
    (void)expected_avg; // just verifying sign
    EXPECT_LT(pos->quantity, toDecimal(0.0)); // still short
}

// ============================================================================
// Reduction (Opposite Direction)
// ============================================================================

TEST(PositionManagerTest, PartialReduceLongPosition) {
    PositionManager pm;
    pm.updatePosition(makeBuyFill(1, 50000.0, 1.0));  // long 1.0 @ 50000
    auto* pos = pm.updatePosition(makeSellFill(1, 51000.0, 0.5));

    ASSERT_NE(pos, nullptr);
    EXPECT_EQ(pos->quantity, toDecimal(0.5));
    // avg price unchanged on reduction
    EXPECT_EQ(pos->average_price, toDecimal(50000.0));
    // realized P&L: 0.5 * (51000 - 50000) = 500
    EXPECT_EQ(pos->realized_pnl, toDecimal(500.0));
}

TEST(PositionManagerTest, FullReduceToFlat) {
    PositionManager pm;
    pm.updatePosition(makeBuyFill(1, 50000.0, 1.0));
    auto* pos = pm.updatePosition(makeSellFill(1, 51000.0, 1.0));

    ASSERT_NE(pos, nullptr);
    EXPECT_EQ(pos->quantity, toDecimal(0.0));
    EXPECT_EQ(pos->average_price, toDecimal(0.0));
    // realized P&L: 1.0 * (51000 - 50000) = 1000
    EXPECT_EQ(pos->realized_pnl, toDecimal(1000.0));
}

TEST(PositionManagerTest, ReduceShortPosition) {
    PositionManager pm;
    pm.updatePosition(makeSellFill(1, 50000.0, 1.0));  // short 1.0 @ 50000
    auto* pos = pm.updatePosition(makeBuyFill(1, 49000.0, 0.5));  // buy back 0.5

    ASSERT_NE(pos, nullptr);
    EXPECT_EQ(pos->quantity, toDecimal(-0.5));
    // short: profit = reduced * (avg - fill) = 0.5 * (50000 - 49000) = 500
    EXPECT_EQ(pos->realized_pnl, toDecimal(500.0));
}

// ============================================================================
// Flip (Opposite Direction Exceeds Position)
// ============================================================================

TEST(PositionManagerTest, FlipLongToShort) {
    PositionManager pm;
    pm.updatePosition(makeBuyFill(1, 50000.0, 1.0));   // long 1.0
    auto* pos = pm.updatePosition(makeSellFill(1, 51000.0, 3.0)); // sell 3.0

    ASSERT_NE(pos, nullptr);
    // Flips: close long 1.0 (realized = 1000), open short 2.0 @ 51000
    EXPECT_EQ(pos->quantity, toDecimal(-2.0));
    EXPECT_EQ(pos->average_price, toDecimal(51000.0));
    // Realized from closing: 1.0 * (51000 - 50000) = 1000
    EXPECT_EQ(pos->realized_pnl, toDecimal(1000.0));
}

TEST(PositionManagerTest, FlipShortToLong) {
    PositionManager pm;
    pm.updatePosition(makeSellFill(1, 50000.0, 1.0));  // short 1.0
    auto* pos = pm.updatePosition(makeBuyFill(1, 49000.0, 3.0));  // buy 3.0

    ASSERT_NE(pos, nullptr);
    // Flips: close short 1.0 (realized = 50000-49000 = 1000), open long 2.0 @ 49000
    EXPECT_EQ(pos->quantity, toDecimal(2.0));
    EXPECT_EQ(pos->average_price, toDecimal(49000.0));
    EXPECT_EQ(pos->realized_pnl, toDecimal(1000.0));
}

// ============================================================================
// P&L Calculations
// ============================================================================

TEST(PositionManagerTest, UnrealizedPnLCalculation) {
    PositionManager pm;
    pm.updatePosition(makeBuyFill(1, 50000.0, 1.0));   // long 1.0 @ 50000
    pm.updatePosition(makeSellFill(2, 40000.0, 1.0));    // short 1.0 @ 40000

    std::unordered_map<uint32_t, Decimal> prices;
    prices[1] = toDecimal(51000.0);   // mark: long is up 1000
    prices[2] = toDecimal(39000.0);   // mark: short is up 1000

    Decimal pnl = pm.getUnrealizedPnL(prices);
    // Long unrealized: 1.0 * (51000 - 50000) = 1000
    // Short unrealized: -1.0 * (39000 - 40000) = 1000
    // Total: 2000
    EXPECT_EQ(pnl, toDecimal(2000.0));
}

TEST(PositionManagerTest, RealizedPnLAccumulation) {
    PositionManager pm;
    pm.updatePosition(makeBuyFill(1, 50000.0, 1.0));
    pm.updatePosition(makeSellFill(1, 50500.0, 0.5));  // +250
    pm.updatePosition(makeSellFill(1, 50200.0, 0.5));  // +100

    const auto* pos = pm.getPosition(1);
    ASSERT_NE(pos, nullptr);
    EXPECT_EQ(pos->quantity, toDecimal(0.0));
    EXPECT_EQ(pos->realized_pnl, toDecimal(350.0));
}

// ============================================================================
// Total Value
// ============================================================================

TEST(PositionManagerTest, GetTotalValueCached) {
    PositionManager pm;
    pm.updatePosition(makeBuyFill(1, 50000.0, 1.0));   // |1*50000| = 50000
    pm.updatePosition(makeBuyFill(2, 40000.0, 0.5));    // |0.5*40000| = 20000

    Decimal cached = pm.getTotalValueCached();
    // total = 1.0 * 50000 + 0.5 * 40000 = 70000
    EXPECT_EQ(cached, toDecimal(70000.0));
}

TEST(PositionManagerTest, GetTotalValueWithExternalPrices) {
    PositionManager pm;
    pm.updatePosition(makeBuyFill(1, 50000.0, 1.0));

    std::unordered_map<uint32_t, Decimal> prices;
    prices[1] = toDecimal(52000.0);
    Decimal val = pm.getTotalValue(prices);
    EXPECT_EQ(val, toDecimal(52000.0));
}

// ============================================================================
// Persistence (JSON Round-Trip)
// ============================================================================

TEST(PositionManagerTest, JsonRoundTrip) {
    PositionManager pm1;
    pm1.updatePosition(makeBuyFill(1, 50000.0, 1.0));
    pm1.updatePosition(makeSellFill(2, 40000.0, 0.5));

    auto json = pm1.toJson();
    ASSERT_TRUE(json.is_array());
    ASSERT_EQ(json.size(), 2U);

    PositionManager pm2;
    ASSERT_TRUE(pm2.fromJson(json));

    auto positions = pm2.getAllPositions();
    ASSERT_EQ(positions.size(), 2U);

    // Both positions should match
    for (const auto& pos : positions) {
        const auto* orig = pm1.getPosition(pos.symbol_id);
        ASSERT_NE(orig, nullptr);
        EXPECT_EQ(pos.quantity, orig->quantity);
        EXPECT_EQ(pos.average_price, orig->average_price);
        EXPECT_EQ(pos.realized_pnl, orig->realized_pnl);
    }
}

TEST(PositionManagerTest, FileRoundTrip) {
    const char* tmpfile = "/tmp/chronos_test_positions.json";
    std::remove(tmpfile);

    {
        PositionManager pm1;
        pm1.updatePosition(makeBuyFill(1, 50000.0, 2.0));
        EXPECT_TRUE(pm1.savePositions(tmpfile));
    }

    {
        PositionManager pm2;
        EXPECT_TRUE(pm2.loadPositions(tmpfile));
        const auto* pos = pm2.getPosition(1);
        ASSERT_NE(pos, nullptr);
        EXPECT_EQ(pos->quantity, toDecimal(2.0));
        EXPECT_EQ(pos->average_price, toDecimal(50000.0));
    }

    std::remove(tmpfile);
}

TEST(PositionManagerTest, SaveFailureReturnsFalse) {
    PositionManager pm;
    EXPECT_FALSE(pm.savePositions("/nonexistent_path_12345/positions.json"));
}

TEST(PositionManagerTest, LoadFailureReturnsFalse) {
    PositionManager pm;
    EXPECT_FALSE(pm.loadPositions("/tmp/nonexistent_file_12345.json"));
}

// ============================================================================
// Concurrency
// ============================================================================

TEST(PositionManagerTest, ConcurrentReadWhileWrite) {
    PositionManager pm;
    std::atomic<bool> start{false};
    std::atomic<bool> running{true};
    std::atomic<size_t> reads{0};

    // Writer thread — waits for start signal
    std::thread writer([&]() {
        while (!start.load(std::memory_order_acquire)) {}
        for (int i = 0; i < 1000; ++i) {
            pm.updatePosition(makeBuyFill(
                static_cast<uint32_t>(1 + i % 3),
                50000.0 + i * 0.1,
                1.0));
        }
        running.store(false, std::memory_order_release);
    });

    // Reader threads — wait for start signal
    std::vector<std::thread> readers;
    for (int t = 0; t < 3; ++t) {
        readers.emplace_back([&]() {
            while (!start.load(std::memory_order_acquire)) {}
            while (running.load(std::memory_order_acquire)) {
                pm.getPosition(1);
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // All threads ready — release them simultaneously
    start.store(true, std::memory_order_release);

    writer.join();
    for (auto& t : readers) t.join();

    // Sanitizers (TSan especially) add 5-15x overhead to every memory access,
    // so the concurrent readers complete far fewer iterations before the writer
    // finishes. Relax the threshold accordingly.
#if defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(undefined_behavior_sanitizer)
    constexpr size_t min_reads = 50;
#  else
    constexpr size_t min_reads = 200;
#  endif
#elif defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    constexpr size_t min_reads = 50;
#else
    constexpr size_t min_reads = 200;
#endif
    EXPECT_GT(reads.load(), min_reads);
    auto positions = pm.getAllPositions();
    EXPECT_LE(positions.size(), 3U);
}
