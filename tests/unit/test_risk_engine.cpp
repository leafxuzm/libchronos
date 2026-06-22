/**
 * @file test_risk_engine.cpp
 * @brief Unit tests for RiskEngine — all 5 risk checks, params, stats, concurrency
 *
 * Validates: Requirements 6.1-6.10
 * PBT Properties 6-11: Risk Check invariants
 */

#include <gtest/gtest.h>
#include <chronos/risk/risk_engine.hpp>
#include <chronos/trading/position_manager.hpp>
#include <chronos/core/config.hpp>
#include <thread>
#include <vector>

using namespace chronos;
using namespace chronos::risk;
using namespace chronos::trading;

namespace {

OrderRequest makeOrder(uint32_t symbol_id, double price, double qty,
                       OrderSide side = OrderSide::BUY) {
    OrderRequest o;
    o.order_id = 1;
    o.symbol_id = symbol_id;
    o.price = toDecimal(price);
    o.quantity = toDecimal(qty);
    o.side = side;
    return o;
}

Fill makeFill(uint32_t symbol_id, double price, double qty,
              OrderSide side = OrderSide::BUY) {
    Fill f;
    f.symbol_id = symbol_id;
    f.fill_price = toDecimal(price);
    f.fill_quantity = toDecimal(qty);
    f.side = side;
    return f;
}

} // anonymous namespace

// ============================================================================
// Constructor
// ============================================================================

TEST(RiskEngineTest, ConstructorInitializesDefaults) {
    PositionManager pm;
    RiskEngine engine(pm);

    auto params = engine.getParameters();
    EXPECT_EQ(params.max_order_value, 100000.0);
    EXPECT_EQ(params.max_orders_per_second, 100U);
}

TEST(RiskEngineTest, ConstructorSetsCapitalToMaxTotalPositionValue) {
    PositionManager pm;
    RiskEngine engine(pm);

    Decimal capital = engine.getAvailableCapital();
    EXPECT_EQ(capital, toDecimal(1000000.0));  // default max_total_position_value
}

// ============================================================================
// Check 1: Order Value
// ============================================================================

TEST(RiskEngineTest, OrderValueCheckPasses) {
    PositionManager pm;
    RiskEngine engine(pm);

    auto result = engine.checkOrder(makeOrder(1, 50000.0, 1.0));
    EXPECT_TRUE(result.passed) << result.rejection_reason;
}

TEST(RiskEngineTest, OrderValueCheckRejects) {
    PositionManager pm;
    RiskEngine engine(pm);

    RiskParameters tight;
    tight.max_order_value = 10.0;  // only 10 USD max
    engine.updateParameters(tight);

    auto result = engine.checkOrder(makeOrder(1, 50000.0, 1.0));
    EXPECT_FALSE(result.passed);
    EXPECT_NE(result.rejection_reason.find("Order value"), std::string::npos);
}

// ============================================================================
// Check 2: Rate Limit
// ============================================================================

TEST(RiskEngineTest, RateLimitCheckRejectsAfterExceeded) {
    PositionManager pm;
    RiskEngine engine(pm);

    RiskParameters tight;
    tight.max_orders_per_second = 2;
    engine.updateParameters(tight);

    // First 2 should pass
    EXPECT_TRUE(engine.checkOrder(makeOrder(1, 50000.0, 1.0)).passed);
    EXPECT_TRUE(engine.checkOrder(makeOrder(1, 50000.0, 1.0)).passed);

    // Third should be rate-limited
    auto result = engine.checkOrder(makeOrder(1, 50000.0, 1.0));
    EXPECT_FALSE(result.passed);
    EXPECT_NE(result.rejection_reason.find("Rate limit"), std::string::npos);
}

// ============================================================================
// Check 3: Position Limit
// ============================================================================

TEST(RiskEngineTest, PositionLimitCheckRejects) {
    PositionManager pm;
    pm.updatePosition(makeFill(1, 50000.0, 1.0));  // existing 1.0 long

    RiskEngine engine(pm);
    RiskParameters tight;
    tight.max_position_value = 1000.0;  // tiny limit
    engine.updateParameters(tight);

    // Adding another 1.0 at 50000 would give position value of 2*50000 = 100000
    auto result = engine.checkOrder(makeOrder(1, 50000.0, 1.0));
    EXPECT_FALSE(result.passed);
    EXPECT_NE(result.rejection_reason.find("Position"), std::string::npos);
}

TEST(RiskEngineTest, PositionLimitCheckPassesWhenUnderLimit) {
    PositionManager pm;
    pm.updatePosition(makeFill(1, 50000.0, 0.1));

    RiskEngine engine(pm);
    RiskParameters wide;
    wide.max_position_value = 100000.0;
    engine.updateParameters(wide);

    auto result = engine.checkOrder(makeOrder(1, 50000.0, 0.1));
    EXPECT_TRUE(result.passed);
}

// ============================================================================
// Check 4: Total Position Value
// ============================================================================

TEST(RiskEngineTest, TotalPositionCheckRejectsWhenExceeded) {
    PositionManager pm;
    pm.updatePosition(makeFill(1, 50000.0, 2.0));  // total value = 100000

    RiskEngine engine(pm);
    RiskParameters tight;
    tight.max_total_position_value = 1000.0;
    engine.updateParameters(tight);

    auto result = engine.checkOrder(makeOrder(2, 50000.0, 0.1));
    EXPECT_FALSE(result.passed);
    EXPECT_NE(result.rejection_reason.find("Total position"), std::string::npos);
}

// ============================================================================
// Check 5: Capital
// ============================================================================

TEST(RiskEngineTest, CapitalCheckRejectsWhenInsufficient) {
    PositionManager pm;
    RiskEngine engine(pm);

    engine.setAvailableCapital(toDecimal(100.0));

    RiskParameters params;
    params.min_available_capital = 10.0;
    engine.updateParameters(params);

    // Order of 1000 would leave capital at -900 (< min_capital of 10)
    auto result = engine.checkOrder(makeOrder(1, 1000.0, 1.0));
    EXPECT_FALSE(result.passed);
    EXPECT_NE(result.rejection_reason.find("capital"), std::string::npos);
}

TEST(RiskEngineTest, CapitalCheckPassesWhenSufficient) {
    PositionManager pm;
    RiskEngine engine(pm);

    engine.setAvailableCapital(toDecimal(100000.0));

    RiskParameters params;
    params.min_available_capital = 100.0;
    engine.updateParameters(params);

    auto result = engine.checkOrder(makeOrder(1, 50000.0, 1.0));
    EXPECT_TRUE(result.passed) << result.rejection_reason;
}

// ============================================================================
// Check Ordering: First Failure Stops Chain
// ============================================================================

TEST(RiskEngineTest, FirstFailureIsOrderValueBeforeRateLimit) {
    PositionManager pm;
    RiskEngine engine(pm);

    RiskParameters tight;
    tight.max_order_value = 1.0;
    tight.max_orders_per_second = 1000;
    engine.updateParameters(tight);

    auto result = engine.checkOrder(makeOrder(1, 50000.0, 1.0));
    EXPECT_FALSE(result.passed);
    // Should fail on order value, not rate limit
    EXPECT_NE(result.rejection_reason.find("Order value"), std::string::npos);
    EXPECT_EQ(result.rejection_reason.find("Rate limit"), std::string::npos);
}

// ============================================================================
// Parameter Updates
// ============================================================================

TEST(RiskEngineTest, UpdateParametersChangesLimits) {
    PositionManager pm;
    RiskEngine engine(pm);

    // Default params: order should pass
    auto r1 = engine.checkOrder(makeOrder(1, 50000.0, 1.0));
    EXPECT_TRUE(r1.passed);

    // Tighten params
    RiskParameters tight;
    tight.max_order_value = 1.0;
    engine.updateParameters(tight);

    auto r2 = engine.checkOrder(makeOrder(1, 50000.0, 1.0));
    EXPECT_FALSE(r2.passed);
}

TEST(RiskEngineTest, GetParametersReturnsCurrent) {
    PositionManager pm;
    RiskEngine engine(pm);

    RiskParameters custom;
    custom.max_order_value = 99999.0;
    custom.max_orders_per_second = 77;
    engine.updateParameters(custom);

    auto params = engine.getParameters();
    EXPECT_EQ(params.max_order_value, 99999.0);
    EXPECT_EQ(params.max_orders_per_second, 77U);
}

// ============================================================================
// Capital
// ============================================================================

TEST(RiskEngineTest, SetAndGetCapital) {
    PositionManager pm;
    RiskEngine engine(pm);

    engine.setAvailableCapital(toDecimal(12345.67));
    EXPECT_EQ(engine.getAvailableCapital(), toDecimal(12345.67));
}

// ============================================================================
// checkRateLimit (Lightweight)
// ============================================================================

TEST(RiskEngineTest, LightweightRateLimitCheck) {
    PositionManager pm;
    RiskEngine engine(pm);

    RiskParameters tight;
    tight.max_orders_per_second = 2;
    engine.updateParameters(tight);

    EXPECT_TRUE(engine.checkRateLimit());
    EXPECT_TRUE(engine.checkRateLimit());
    EXPECT_FALSE(engine.checkRateLimit());
}

// ============================================================================
// Statistics
// ============================================================================

TEST(RiskEngineTest, StatisticsTrackAcceptanceAndRejection) {
    PositionManager pm;
    RiskEngine engine(pm);

    RiskParameters tight;
    tight.max_order_value = 10.0;
    engine.updateParameters(tight);

    engine.checkOrder(makeOrder(1, 100.0, 1.0));  // rejected (order value)
    engine.checkOrder(makeOrder(1, 5.0, 1.0));    // accepted

    auto stats = engine.getStatistics();
    EXPECT_EQ(stats.total_checks, 2U);
    EXPECT_GE(stats.accepted, 1U);
    EXPECT_GE(stats.rejected, 1U);
}

TEST(RiskEngineTest, StatisticsTracksRejectionCategories) {
    PositionManager pm;
    RiskEngine engine(pm);

    RiskParameters tight;
    tight.max_order_value = 10.0;
    engine.updateParameters(tight);

    engine.checkOrder(makeOrder(1, 50000.0, 1.0));  // order value

    auto stats = engine.getStatistics();
    EXPECT_GE(stats.order_value_rejects, 1U);
}

TEST(RiskEngineTest, ResetStatisticsClearsAll) {
    PositionManager pm;
    RiskEngine engine(pm);

    engine.checkOrder(makeOrder(1, 50000.0, 1.0));

    engine.resetStatistics();

    auto stats = engine.getStatistics();
    EXPECT_EQ(stats.total_checks, 0U);
    EXPECT_EQ(stats.accepted, 0U);
    EXPECT_EQ(stats.rejected, 0U);
}

// ============================================================================
// Concurrency
// ============================================================================

TEST(RiskEngineTest, ConcurrentCheckOrders) {
    PositionManager pm;
    pm.updatePosition(makeFill(1, 50000.0, 0.1));
    RiskEngine engine(pm);

    RiskParameters wide;
    wide.max_order_value = 1000000.0;
    wide.max_orders_per_second = 100000;
    wide.max_position_value = 1000000.0;
    engine.updateParameters(wide);

    std::atomic<int> passed{0}, failed{0};
    constexpr int PER_THREAD = 500;
    constexpr int THREADS = 4;

    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < PER_THREAD; ++i) {
                auto r = engine.checkOrder(makeOrder(1, 50000.0, 0.01));
                if (r.passed) passed.fetch_add(1, std::memory_order_relaxed);
                else failed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& t : threads) t.join();

    EXPECT_GE(passed.load(), 1);
    auto stats = engine.getStatistics();
    EXPECT_EQ(stats.total_checks, static_cast<uint64_t>(PER_THREAD * THREADS));
}
