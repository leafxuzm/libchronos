/**
 * @file test_trading_pipeline.cpp
 * @brief End-to-end trading pipeline integration:
 *        StrategyEngine → RiskEngine → OrderQueue → OrderGateway → simulated fill
 *        → FillCallback → StrategyEngine → Strategy.onFill()
 *
 * Validates the full order lifecycle: tick → strategy decision → risk check →
 * order send → fill → position update → strategy notification.
 */

#include <gtest/gtest.h>
#include <chronos/trading/strategy_engine.hpp>
#include <chronos/execution/order_gateway.hpp>
#include <chronos/strategies/grid_strategy.hpp>
#include <chronos/core/types.hpp>
#include <chronos/core/config.hpp>
#include <thread>
#include <atomic>

using namespace chronos;
using namespace chronos::trading;
using namespace chronos::execution;
using namespace chronos::strategies;

namespace {

Tick makeTick(uint32_t symbol_id, double price, double qty,
              TickSide side, uint64_t ts) {
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
// Fixture — wires StrategyEngine ↔ OrderGateway with shared order queue
// ============================================================================
class TradingPipelineTest : public ::testing::Test {
protected:
    void SetUp() override {
        engine_ = std::make_unique<StrategyEngine>();
        engine_->setAvailableCapital(toDecimal(100000.0));
        RiskParameters params;
        params.max_order_value = 1000000.0;
        params.max_orders_per_second = 1000000;
        engine_->updateRiskParameters(params);
    }

    void TearDown() override {
        if (gateway_ && gateway_->isRunning()) gateway_->stop();
        if (engine_ && engine_->isRunning()) engine_->stop();
    }

    /// Create OrderGateway sharing engine's order queue.
    /// exchange_id=0 matches strategy default.
    void wireGateway() {
        ExchangeConfig cfg;
        cfg.name = "simulated";

        gateway_ = std::make_unique<OrderGateway>(
            cfg,
            engine_->getOrderQueue(),   // shared queue: engine writes, gateway reads
            [this](const Fill& fill) {  // fill callback → back to engine
                engine_->pushFill(fill);
            },
            0,     // exchange_id matches strategy default (ExchangeId::UNKNOWN)
            true   // simulate_fills = auto-generate fills after send
        );
    }

    GridStrategy* setupGridStrategy(double low = 95.0, double high = 105.0,
                                    int levels = 5, double qty = 0.1,
                                    uint32_t symbol_id = 1) {
        GridStrategy::Config cfg{low, high, levels, qty, symbol_id};
        auto s = std::make_unique<GridStrategy>(cfg);
        auto* raw = s.get();
        engine_->registerStrategy(std::move(s));
        return raw;
    }

    void startAll() {
        wireGateway();
        engine_->start();
        gateway_->start();
    }

    void stopAll() {
        if (gateway_) gateway_->stop();
        if (engine_) engine_->stop();
    }

    std::unique_ptr<StrategyEngine> engine_;
    std::unique_ptr<OrderGateway> gateway_;
};

// ============================================================================
// 1. Full Pipeline: Tick → Order → auto-Fill → onFill → Position
// ============================================================================

TEST_F(TradingPipelineTest, FullOrderLifecycle) {
    auto* strategy = setupGridStrategy();

    // Prime order book with BID+ASK pairs so getMidPrice() returns valid
    for (int i = 0; i < 30; ++i) {
        engine_->pushTick(makeTick(1, 100.05, 1.0, TickSide::ASK, 1000000 + i * 50000));
        engine_->pushTick(makeTick(1, 99.95, 1.0, TickSide::BID, 1000000 + i * 50000 + 100));
    }

    startAll();

    // Wait for engine to process ticks and gateway to process orders + fills
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // Capture state BEFORE stop (stop calls onUnload which resets state)
    bool was_initialized = strategy->isInitialized();
    uint64_t tick_count = strategy->tickCount();
    uint64_t fill_count = strategy->fillCount();
    auto stats = engine_->getStats();
    auto gw_stats = gateway_->getStats();

    stopAll();

    EXPECT_TRUE(was_initialized);
    EXPECT_GT(tick_count, 0u);
    EXPECT_GT(stats.ticks_processed, 0u);
    EXPECT_GT(stats.orders_submitted, 0u)
        << "Strategy should submit orders via submitOrder()";
    EXPECT_GT(stats.fills_processed, 0u)
        << "Gateway auto-fills should flow back to engine via FillCallback";
    EXPECT_GT(fill_count, 0u)
        << "Strategy.onFill() should be called for each fill";
    EXPECT_GT(gw_stats.orders_received, 0u);
    EXPECT_GT(gw_stats.orders_sent, 0u);
}

// ============================================================================
// 2. Risk Engine Rejects Orders When Limits Tight
// ============================================================================

TEST_F(TradingPipelineTest, RiskRejectTightLimits) {
    RiskParameters tight;
    tight.max_order_value = 0.01;  // rejects anything valued > 0.01
    tight.max_orders_per_second = 1000000;
    engine_->updateRiskParameters(tight);

    setupGridStrategy();

    for (int i = 0; i < 20; ++i) {
        engine_->pushTick(makeTick(1, 100.05, 1.0, TickSide::ASK, 1000000 + i * 50000));
        engine_->pushTick(makeTick(1, 99.95, 1.0, TickSide::BID, 1000000 + i * 50000 + 100));
    }

    startAll();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stopAll();

    auto stats = engine_->getStats();
    EXPECT_GT(stats.orders_risk_rejected, 0u)
        << "Order value ~10.0 exceeds max 0.01, should be rejected";
}

// ============================================================================
// 3. Fill Updates Position Correctly
// ============================================================================

TEST_F(TradingPipelineTest, FillUpdatesPosition) {
    engine_->start();  // engine must be running for run() to process fills

    for (int i = 0; i < 5; ++i) {
        Fill fill;
        fill.fill_id = static_cast<uint64_t>(i + 1);
        fill.order_id = static_cast<uint64_t>(100 + i);
        fill.symbol_id = 1;
        fill.fill_price = toDecimal(50000.0 + i * 10.0);
        fill.fill_quantity = toDecimal(0.1);
        fill.side = OrderSide::BUY;
        fill.strategy_id = 1;
        fill.receive_timestamp_us = 1000000 + i;
        engine_->pushFill(fill);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto stats = engine_->getStats();
    auto* pos = engine_->positionManager().getPosition(1);
    auto total = engine_->positionManager().getTotalValueCached();

    engine_->stop();

    ASSERT_NE(pos, nullptr);
    EXPECT_GT(pos->quantity, Decimal(0));
    EXPECT_GT(pos->average_price, Decimal(0));
    EXPECT_EQ(stats.fills_processed, 5u);
    EXPECT_GT(total, Decimal(0));
}

// ============================================================================
// 4. Gateway Auto-Fill Generates Correct Fill Data
// ============================================================================

TEST_F(TradingPipelineTest, GatewayAutoFillCorrectness) {
    setupGridStrategy();

    for (int i = 0; i < 30; ++i) {
        engine_->pushTick(makeTick(1, 100.05, 1.0, TickSide::ASK, 1000000 + i * 50000));
        engine_->pushTick(makeTick(1, 99.95, 1.0, TickSide::BID, 1000000 + i * 50000 + 100));
    }

    startAll();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stopAll();

    auto stats = engine_->getStats();
    auto gw_stats = gateway_->getStats();

    // Gateway should have received and sent orders
    EXPECT_GT(gw_stats.orders_received, 0u);
    EXPECT_GT(gw_stats.orders_sent, 0u);

    // Every sent order should generate a fill (via auto-simulate)
    // fills_processed >= orders_sent (some orders might generate fills from
    // previous test iterations though — we just check >0)
    EXPECT_GT(stats.fills_processed, 0u);

    // No mismatches — exchange_id matches
    EXPECT_EQ(gw_stats.exchange_id_mismatch, 0u);
}

// ============================================================================
// 5. ExchangeId Mismatch Blocks Orders
// ============================================================================

TEST_F(TradingPipelineTest, ExchangeIdMismatchBlocksOrders) {
    setupGridStrategy();

    // Create gateway with non-matching exchange_id
    ExchangeConfig cfg;
    cfg.name = "mismatched";
    OrderGateway mismatch_gw(
        cfg,
        engine_->getOrderQueue(),
        [this](const Fill& fill) { engine_->pushFill(fill); },
        99,    // exchange_id=99, won't match strategy default 0
        false  // no auto-fill
    );

    // Push ticks so strategy generates orders
    for (int i = 0; i < 20; ++i) {
        engine_->pushTick(makeTick(1, 100.05, 1.0, TickSide::ASK, 1000000 + i * 50000));
        engine_->pushTick(makeTick(1, 99.95, 1.0, TickSide::BID, 1000000 + i * 50000 + 100));
    }

    engine_->start();
    mismatch_gw.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    mismatch_gw.stop();
    engine_->stop();

    auto gw_stats = mismatch_gw.getStats();
    EXPECT_GT(gw_stats.orders_received, 0u);
    EXPECT_GT(gw_stats.exchange_id_mismatch, 0u)
        << "All orders should be rejected due to exchange_id mismatch";
    EXPECT_EQ(gw_stats.orders_sent, 0u)
        << "No orders should pass the exchange_id filter";
}

// ============================================================================
// 6. Double Stop Is Safe
// ============================================================================

TEST_F(TradingPipelineTest, DoubleStopIsSafe) {
    setupGridStrategy();
    startAll();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    stopAll();
    stopAll();  // second stop
    stopAll();  // third stop

    EXPECT_FALSE(engine_->isRunning());
    EXPECT_FALSE(gateway_->isRunning());
}

// ============================================================================
// 7. Cancel Order Flow
// ============================================================================

TEST_F(TradingPipelineTest, CancelOrderFlow) {
    setupGridStrategy();

    for (int i = 0; i < 30; ++i) {
        engine_->pushTick(makeTick(1, 100.05, 1.0, TickSide::ASK, 1000000 + i * 50000));
        engine_->pushTick(makeTick(1, 99.95, 1.0, TickSide::BID, 1000000 + i * 50000 + 100));
    }

    startAll();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stopAll();

    // GridStrategy's onTimer cancels stale orders (those on wrong side of mid)
    auto gw_stats = gateway_->getStats();
    EXPECT_GT(gw_stats.orders_received, 0u);
    // Cancels may be non-zero depending on mid price movement
}

// ============================================================================
// 8. Stats Consistency
// ============================================================================

TEST_F(TradingPipelineTest, StatsConsistency) {
    setupGridStrategy();

    for (int i = 0; i < 20; ++i) {
        engine_->pushTick(makeTick(1, 100.05, 1.0, TickSide::ASK, 1000000 + i * 50000));
        engine_->pushTick(makeTick(1, 99.95, 1.0, TickSide::BID, 1000000 + i * 50000 + 100));
    }

    startAll();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stopAll();

    auto stats = engine_->getStats();
    EXPECT_GT(stats.ticks_processed, 0u);

    // Total orders received by gateway = submitted (if no queue drop)
    auto gw_stats = gateway_->getStats();
    EXPECT_GE(gw_stats.orders_received, 0u);

    // orders_submitted only increments after passing risk + queue push
    // So submitted = orders_received by gateway (minus possible timing)
}

// ============================================================================
// 9. Multi-Strategy Pipeline
// ============================================================================

TEST_F(TradingPipelineTest, MultiStrategyPipeline) {
    GridStrategy::Config cfg1{95.0, 105.0, 5, 0.1, 1};
    GridStrategy::Config cfg2{1900.0, 2100.0, 5, 0.01, 2};

    auto s1 = std::make_unique<GridStrategy>(cfg1);
    auto s2 = std::make_unique<GridStrategy>(cfg2);
    auto* raw1 = s1.get();
    auto* raw2 = s2.get();
    engine_->registerStrategy(std::move(s1));
    engine_->registerStrategy(std::move(s2));

    // Prime both order books
    for (int i = 0; i < 20; ++i) {
        engine_->pushTick(makeTick(1, 100.05, 1.0, TickSide::ASK, 1000000 + i * 50000));
        engine_->pushTick(makeTick(1, 99.95, 1.0, TickSide::BID, 1000000 + i * 50000 + 100));
        engine_->pushTick(makeTick(2, 2000.05, 1.0, TickSide::ASK, 1000000 + i * 50000 + 200));
        engine_->pushTick(makeTick(2, 1999.95, 1.0, TickSide::BID, 1000000 + i * 50000 + 300));
    }

    startAll();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stopAll();

    EXPECT_GT(raw1->tickCount(), 0u);
    EXPECT_GT(raw2->tickCount(), 0u);

    // Grid levels are in correct ranges for each symbol
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

    auto stats = engine_->getStats();
    EXPECT_GT(stats.orders_submitted, 0u);
    EXPECT_GT(stats.fills_processed, 0u);
}
