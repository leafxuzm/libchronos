/**
 * @file test_system.cpp
 * @brief System-level tests — config-driven setup, full pipeline, graceful shutdown
 *
 * Validates: Requirements 21.1-21.7 (system lifecycle, config integration, graceful shutdown)
 */

#include <gtest/gtest.h>
#include <chronos/trading/strategy_engine.hpp>
#include <chronos/core/types.hpp>
#include <chronos/core/config.hpp>
#include <chronos/strategies/grid_strategy.hpp>
#include <chronos/market_data/orderbook_v2.hpp>
#include <thread>
#include <atomic>
#include <fstream>
#include <filesystem>

using namespace chronos;
using namespace chronos::trading;
using namespace chronos::strategies;
using namespace chronos::market_data;

namespace fs = std::filesystem;

namespace {

const std::string TEST_DIR = "/tmp/chronos_system_test";

void cleanDir() {
    std::error_code ec;
    if (fs::exists(TEST_DIR, ec)) fs::remove_all(TEST_DIR, ec);
    fs::create_directories(TEST_DIR);
}

Tick makeTick(uint32_t symbol_id, double price, double qty,
              TickSide side = TickSide::BID, uint64_t ts = 1000000) {
    Tick t;
    t.symbol_id = symbol_id;
    t.price = toDecimal(price);
    t.quantity = toDecimal(qty);
    t.side = side;
    t.receive_timestamp_us = ts;
    t.exchange_timestamp_us = ts;
    return t;
}

} // anonymous namespace

// ============================================================================
// 1. Config-Driven Pipeline — GridStrategy from config parameters
// ============================================================================

TEST(SystemTest, ConfigDrivenGridStrategyPipeline) {
    // Simulate reading config and setting up components
    RiskParameters risk_params;
    risk_params.max_order_value = 100000.0;
    risk_params.max_position_value = 500000.0;
    risk_params.max_orders_per_second = 1000;

    StrategyEngine engine;
    engine.setAvailableCapital(toDecimal(100000.0));
    engine.updateRiskParameters(risk_params);

    // Config-driven GridStrategy setup (mirrors system_config.json)
    GridStrategy::Config grid_cfg;
    grid_cfg.symbol_id = 1;
    grid_cfg.grid_low = 95.0;
    grid_cfg.grid_high = 105.0;
    grid_cfg.grid_levels = 5;
    grid_cfg.quantity = 0.1;

    auto strategy = std::make_unique<GridStrategy>(grid_cfg);
    auto* raw = strategy.get();
    engine.registerStrategy(std::move(strategy));
    engine.start();

    // Push ticks to populate order book (BID+ASK for getMidPrice)
    for (int i = 0; i < 30; ++i) {
        double base = 100.0;
        if (i >= 15) base = 96.0;

        engine.pushTick(makeTick(1, base + 0.05, 1.0, TickSide::ASK, 1000000 + i * 50000));
        engine.pushTick(makeTick(1, base - 0.05, 1.0, TickSide::BID, 1000000 + i * 50000 + 100));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Drain orders
    size_t order_count = 0;
    OrderRequest order;
    while (engine.popOrder(order)) {
        if (order.quantity != Decimal(0)) order_count++;
    }

    // Verify strategy was initialized and processed ticks
    EXPECT_TRUE(raw->isInitialized());
    EXPECT_GT(raw->tickCount(), 0u) << "GridStrategy should have processed ticks";
    EXPECT_GT(order_count, 0u) << "GridStrategy should have placed orders";

    // Verify grid levels match config
    EXPECT_EQ(raw->config().grid_low, 95.0);
    EXPECT_EQ(raw->config().grid_high, 105.0);
    EXPECT_EQ(raw->config().grid_levels, 5);
    EXPECT_EQ(raw->levels().size(), 6u);  // grid_levels + 1

    auto stats = engine.getStats();
    EXPECT_GT(stats.ticks_processed, 0u);
    EXPECT_GT(stats.orders_submitted, 0u);

    engine.stop();
}

// ============================================================================
// 2. Risk Parameter Hot Reload — verify parameters update without restart
// ============================================================================

TEST(SystemTest, RiskParameterHotReload) {
    StrategyEngine engine;
    engine.setAvailableCapital(toDecimal(100000.0));

    // Initial tight parameters
    RiskParameters tight;
    tight.max_order_value = 1.0;  // rejects nearly everything
    tight.max_orders_per_second = 1000000;
    engine.updateRiskParameters(tight);

    auto strategy = std::make_unique<GridStrategy>(
        GridStrategy::Config{95.0, 105.0, 5, 0.1, 1});
    auto* raw = strategy.get();
    engine.registerStrategy(std::move(strategy));
    engine.start();

    // Prime the order book
    engine.pushTick(makeTick(1, 100.05, 1.0, TickSide::ASK));
    engine.pushTick(makeTick(1, 99.95, 1.0, TickSide::BID));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Drain any orders generated with tight params
    OrderRequest dummy;
    while (engine.popOrder(dummy)) {}

    uint64_t orders_before = raw->tickCount();

    // Push another tick with tight params — orders should be risk-rejected
    engine.pushTick(makeTick(1, 100.05, 1.0, TickSide::ASK));
    engine.pushTick(makeTick(1, 99.95, 1.0, TickSide::BID));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    size_t orders_with_tight = 0;
    while (engine.popOrder(dummy)) {
        if (dummy.quantity != Decimal(0)) orders_with_tight++;
    }

    // Now hot-reload with generous parameters
    RiskParameters generous;
    generous.max_order_value = 1000000.0;
    generous.max_orders_per_second = 1000000;
    engine.updateRiskParameters(generous);

    // Push another tick — orders should now pass
    engine.pushTick(makeTick(1, 100.05, 1.0, TickSide::ASK));
    engine.pushTick(makeTick(1, 99.95, 1.0, TickSide::BID));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    size_t orders_with_generous = 0;
    while (engine.popOrder(dummy)) {
        if (dummy.quantity != Decimal(0)) orders_with_generous++;
    }

    engine.stop();

    EXPECT_GT(raw->tickCount(), orders_before) << "Strategy should have processed more ticks";
    EXPECT_GT(orders_with_generous, orders_with_tight)
        << "Generous params should allow more orders than tight params";
}

// ============================================================================
// 3. Graceful Shutdown — no data loss, positions preserved
// ============================================================================

TEST(SystemTest, GracefulShutdownPreservesState) {
    StrategyEngine engine;
    engine.setAvailableCapital(toDecimal(100000.0));

    RiskParameters params;
    params.max_order_value = 1000000.0;
    params.max_orders_per_second = 1000000;
    engine.updateRiskParameters(params);

    auto strategy = std::make_unique<GridStrategy>(
        GridStrategy::Config{95.0, 105.0, 5, 0.1, 1});
    engine.registerStrategy(std::move(strategy));
    engine.start();

    // Push fills to accumulate positions
    for (int i = 0; i < 10; ++i) {
        Fill fill;
        fill.fill_id = static_cast<uint64_t>(i + 1);
        fill.order_id = static_cast<uint64_t>(100 + i);
        fill.symbol_id = 1;
        fill.fill_price = toDecimal(50000.0 + i * 10.0);
        fill.fill_quantity = toDecimal(0.1);
        fill.side = OrderSide::BUY;
        fill.strategy_id = 1;
        fill.receive_timestamp_us = 1000000 + i;

        engine.pushFill(fill);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Shutdown
    engine.stop();

    // After stop, position manager state should be intact
    auto* pos = engine.positionManager().getPosition(1);
    ASSERT_NE(pos, nullptr);
    EXPECT_GT(pos->quantity, Decimal(0)) << "Position should be preserved after shutdown";
    EXPECT_GT(pos->average_price, Decimal(0)) << "Average price should be preserved";

    // Double stop should be safe
    engine.stop();

    // Position should still be accessible
    pos = engine.positionManager().getPosition(1);
    ASSERT_NE(pos, nullptr);
}

// ============================================================================
// 4. Multi-Symbol Isolation — symbols don't interfere with each other
// ============================================================================

TEST(SystemTest, MultiSymbolIsolation) {
    StrategyEngine engine;
    engine.setAvailableCapital(toDecimal(100000.0));

    RiskParameters params;
    params.max_order_value = 1000000.0;
    params.max_orders_per_second = 1000000;
    engine.updateRiskParameters(params);

    // Two GridStrategies for different symbols
    GridStrategy::Config cfg1{95.0, 105.0, 5, 0.1, 1};
    GridStrategy::Config cfg2{1900.0, 2100.0, 5, 0.01, 2};

    auto s1 = std::make_unique<GridStrategy>(cfg1);
    auto s2 = std::make_unique<GridStrategy>(cfg2);
    auto* raw1 = s1.get();
    auto* raw2 = s2.get();
    engine.registerStrategy(std::move(s1));
    engine.registerStrategy(std::move(s2));
    engine.start();

    // Push ticks for both symbols
    for (int i = 0; i < 20; ++i) {
        engine.pushTick(makeTick(1, 100.05, 1.0, TickSide::ASK, 1000000 + i * 50000));
        engine.pushTick(makeTick(1, 99.95, 1.0, TickSide::BID, 1000000 + i * 50000 + 100));
        engine.pushTick(makeTick(2, 2000.05, 1.0, TickSide::ASK, 1000000 + i * 50000 + 200));
        engine.pushTick(makeTick(2, 1999.95, 1.0, TickSide::BID, 1000000 + i * 50000 + 300));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    engine.stop();

    // Both strategies should have processed ticks
    EXPECT_GT(raw1->tickCount(), 0u) << "Strategy 1 should have processed ticks";
    EXPECT_GT(raw2->tickCount(), 0u) << "Strategy 2 should have processed ticks";

    // Each strategy should only have levels for its own symbol
    for (auto& level : raw1->levels()) {
        double p = toDouble(level.price);
        EXPECT_GE(p, 95.0);
        EXPECT_LE(p, 105.0);
    }
    for (auto& level : raw2->levels()) {
        double p = toDouble(level.price);
        EXPECT_GE(p, 1900.0);
        EXPECT_LE(p, 2100.0);
    }
}

// ============================================================================
// 5. Stats Consistency — counters are monotonic and consistent
// ============================================================================

TEST(SystemTest, StatsMonotonicAndConsistent) {
    StrategyEngine engine;
    engine.setAvailableCapital(toDecimal(100000.0));

    RiskParameters params;
    params.max_order_value = 1000000.0;
    params.max_orders_per_second = 1000000;
    engine.updateRiskParameters(params);

    auto strategy = std::make_unique<GridStrategy>(
        GridStrategy::Config{95.0, 105.0, 5, 0.1, 1});
    engine.registerStrategy(std::move(strategy));
    engine.start();

    // Push ticks in phases, check stats between each
    for (int phase = 0; phase < 3; ++phase) {
        for (int i = 0; i < 10; ++i) {
            engine.pushTick(makeTick(1, 100.05 + i * 0.01, 1.0, TickSide::ASK,
                                     1000000 + phase * 1000000 + i * 50000));
            engine.pushTick(makeTick(1, 99.95 - i * 0.01, 1.0, TickSide::BID,
                                     1000000 + phase * 1000000 + i * 50000 + 100));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        auto stats = engine.getStats();
        EXPECT_GT(stats.ticks_processed, phase * 20u)
            << "Tick count should increase after phase " << phase;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    engine.stop();

    auto final_stats = engine.getStats();
    EXPECT_GE(final_stats.ticks_processed, 60u);
    // orders_submitted >= orders_risk_rejected + orders_queue_dropped is always true
    // since submitted goes up only after passing risk and queue push
}
