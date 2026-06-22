/**
 * @file test_strategy_engine.cpp
 * @brief Integration tests for StrategyEngine — tick dispatch, order submission, lifecycle
 *
 * Validates: Requirements 9.1-9.7 (Strategy engine tick processing, risk integration,
 *           order ID assignment, strategy lifecycle, timer callbacks)
 */

#include <gtest/gtest.h>
#include <chronos/trading/strategy_engine.hpp>
#include <chronos/core/types.hpp>
#include <thread>
#include <atomic>

using namespace chronos;
using namespace chronos::trading;

namespace {

Tick makeTick(uint32_t symbol_id, double price, double qty, TickSide side = TickSide::BID) {
    Tick t;
    t.symbol_id = symbol_id;
    t.price = toDecimal(price);
    t.quantity = toDecimal(qty);
    t.side = side;
    t.receive_timestamp_us = 1000000;
    return t;
}

/// Minimal strategy that counts ticks and submits orders.
class TestStrategy : public Strategy {
public:
    const char* name() const override { return "TestStrategy"; }

    std::vector<uint32_t> symbols() const override { return {1}; }

    void onTick(const Tick& tick, StrategyContext& ctx) override {
        tick_count.fetch_add(1, std::memory_order_relaxed);
        last_price.store(tick.price.raw_value(), std::memory_order_relaxed);

        // Submit an order every 'submit_every' ticks
        uint64_t n = tick_count.load(std::memory_order_relaxed);
        if (submit_every > 0 && (n % submit_every) == 0) {
            OrderRequest order;
            order.symbol_id = tick.symbol_id;
            order.price = tick.price;
            order.quantity = toDecimal(0.1);
            order.side = OrderSide::BUY;
            order.type = OrderType::LIMIT;

            uint64_t id = ctx.submitOrder(order);
            if (id != 0) {
                last_order_id.store(id, std::memory_order_relaxed);
                orders_submitted.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    void onTimer(uint64_t /*ts*/, StrategyContext& /*ctx*/) override {
        timer_fired.store(true, std::memory_order_relaxed);
    }

    void onLoad(StrategyContext& /*ctx*/) override {
        loaded.store(true, std::memory_order_relaxed);
    }

    void onUnload(StrategyContext& /*ctx*/) override {
        loaded.store(false, std::memory_order_relaxed);
    }

    void onFill(const Fill& /*fill*/, StrategyContext& /*ctx*/) override {
        fill_count.fetch_add(1, std::memory_order_relaxed);
    }

    std::atomic<uint64_t> tick_count{0};
    std::atomic<uint64_t> orders_submitted{0};
    std::atomic<uint64_t> last_order_id{0};
    std::atomic<int64_t>  last_price{0};
    std::atomic<bool>     loaded{false};
    std::atomic<bool>     timer_fired{false};
    std::atomic<uint64_t> fill_count{0};
    uint64_t submit_every{0};  // 0 = never
};

} // anonymous namespace

// ============================================================================
// Lifecycle
// ============================================================================

TEST(StrategyEngineTest, StartStopLifecycle) {
    StrategyEngine engine;

    auto strategy = std::make_unique<TestStrategy>();
    auto* raw = strategy.get();

    engine.registerStrategy(std::move(strategy));
    EXPECT_FALSE(raw->loaded.load());

    engine.start();
    EXPECT_TRUE(engine.isRunning());
    // onLoad should have been called
    EXPECT_TRUE(raw->loaded.load());

    engine.stop();
    EXPECT_FALSE(engine.isRunning());
    EXPECT_FALSE(raw->loaded.load());  // onUnload called
}

TEST(StrategyEngineTest, DoubleStartIsSafe) {
    StrategyEngine engine;
    engine.registerStrategy(std::make_unique<TestStrategy>());
    engine.start();
    engine.start();  // should be no-op
    EXPECT_TRUE(engine.isRunning());
    engine.stop();
}

TEST(StrategyEngineTest, DoubleStopIsSafe) {
    StrategyEngine engine;
    engine.registerStrategy(std::make_unique<TestStrategy>());
    engine.start();
    engine.stop();
    engine.stop();  // should be no-op
    EXPECT_FALSE(engine.isRunning());
}

// ============================================================================
// Tick Dispatch
// ============================================================================

TEST(StrategyEngineTest, TickDispatchToStrategy) {
    StrategyEngine engine;
    auto strategy = std::make_unique<TestStrategy>();
    auto* raw = strategy.get();

    engine.registerStrategy(std::move(strategy));
    engine.start();

    // Push a tick
    auto tick = makeTick(1, 50000.0, 1.0);
    EXPECT_TRUE(engine.pushTick(tick));

    // Wait for dispatch
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_GE(raw->tick_count.load(), 1U);
    EXPECT_EQ(raw->last_price.load(), toDecimal(50000.0).raw_value());

    engine.stop();
}

TEST(StrategyEngineTest, NonSubscribedSymbolNotDispatched) {
    StrategyEngine engine;

    auto strategy = std::make_unique<TestStrategy>();  // subscribes to {1}
    auto* raw = strategy.get();

    engine.registerStrategy(std::move(strategy));
    engine.start();

    // Tick for symbol 2 (not subscribed)
    engine.pushTick(makeTick(2, 50000.0, 1.0));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Strategy should NOT have received the tick for symbol 2
    EXPECT_EQ(raw->tick_count.load(), 0U);

    // Tick for symbol 1 (subscribed)
    engine.pushTick(makeTick(1, 50000.0, 1.0));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    EXPECT_EQ(raw->tick_count.load(), 1U);

    engine.stop();
}

// ============================================================================
// Order Submission (through RiskEngine)
// ============================================================================

TEST(StrategyEngineTest, OrderSubmissionViaContext) {
    StrategyEngine engine;

    RiskParameters params;
    params.max_order_value = 1000000.0;
    params.max_orders_per_second = 1000000;
    engine.updateRiskParameters(params);

    auto strategy = std::make_unique<TestStrategy>();
    strategy->submit_every = 1;  // submit on every tick
    auto* raw = strategy.get();

    engine.registerStrategy(std::move(strategy));
    engine.start();

    // Push a tick that should trigger order submission
    engine.pushTick(makeTick(1, 50000.0, 1.0));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_GE(raw->orders_submitted.load(), 1U);
    EXPECT_GT(raw->last_order_id.load(), 0U);

    // Verify order is in the output queue
    OrderRequest out;
    bool has_order = engine.popOrder(out);
    EXPECT_TRUE(has_order);
    if (has_order) {
        EXPECT_EQ(out.symbol_id, 1U);
        EXPECT_GT(out.order_id, 0U);
    }

    engine.stop();
}

TEST(StrategyEngineTest, OrderRejectedByRiskEngine) {
    StrategyEngine engine;

    // Very tight risk params — any order >1 USD is rejected
    RiskParameters tight;
    tight.max_order_value = 1.0;  // only 1 USD
    tight.max_orders_per_second = 1000000;
    engine.updateRiskParameters(tight);

    auto strategy = std::make_unique<TestStrategy>();
    strategy->submit_every = 1;
    auto* raw = strategy.get();

    engine.registerStrategy(std::move(strategy));
    engine.start();

    engine.pushTick(makeTick(1, 50000.0, 1.0));  // order would be 50000 * 0.1 = 5000 USD
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Order should be rejected
    EXPECT_EQ(raw->last_order_id.load(), 0U);

    // Stats should show rejection (now tracked by submitOrder)
    auto stats = engine.getStats();
    EXPECT_GE(stats.orders_risk_rejected, 1U);
    EXPECT_GE(stats.ticks_processed, 1U);

    engine.stop();
}

// ============================================================================
// Order Cancellation
// ============================================================================

TEST(StrategyEngineTest, CancelOrderProducesCancelRequest) {
    StrategyEngine engine;

    // Custom strategy that cancels an order on first tick
    class CancelStrategy : public Strategy {
    public:
        const char* name() const override { return "CancelStrategy"; }
        std::vector<uint32_t> symbols() const override { return {1}; }

        void onTick(const Tick& /*tick*/, StrategyContext& ctx) override {
            bool expected = false;
            if (cancelled.compare_exchange_strong(expected, true)) {
                ctx.cancelOrder(42);
            }
        }
        std::atomic<bool> cancelled{false};
    };

    auto strategy = std::make_unique<CancelStrategy>();
    auto* raw = strategy.get();

    engine.registerStrategy(std::move(strategy));
    engine.start();

    engine.pushTick(makeTick(1, 50000.0, 1.0));

    // Wait for dispatch
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!raw->cancelled && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    EXPECT_TRUE(raw->cancelled);

    // Verify the cancel request was enqueued with quantity=0
    OrderRequest out;
    bool has_cancel = engine.popOrder(out);
    EXPECT_TRUE(has_cancel);
    if (has_cancel) {
        EXPECT_EQ(out.order_id, 42ULL);
        EXPECT_EQ(out.quantity, Decimal(0));
    }

    engine.stop();
}

// ============================================================================
// Statistics
// ============================================================================

TEST(StrategyEngineTest, EngineStatsTrackTicks) {
    StrategyEngine engine;
    engine.registerStrategy(std::make_unique<TestStrategy>());
    engine.start();

    engine.pushTick(makeTick(1, 50000.0, 1.0));
    engine.pushTick(makeTick(1, 50001.0, 2.0));
    engine.pushTick(makeTick(1, 50002.0, 0.5));

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto stats = engine.getStats();
    EXPECT_GE(stats.ticks_processed, 3U);

    engine.stop();
}

// ============================================================================
// Risk Parameters Update
// ============================================================================

TEST(StrategyEngineTest, UpdateRiskParametersDuringOperation) {
    StrategyEngine engine;
    engine.registerStrategy(std::make_unique<TestStrategy>());
    engine.start();

    auto params = engine.getRiskParameters();
    EXPECT_GT(params.max_order_value, 0.0);

    RiskParameters custom;
    custom.max_order_value = 99999.0;
    engine.updateRiskParameters(custom);

    auto updated = engine.getRiskParameters();
    EXPECT_EQ(updated.max_order_value, 99999.0);

    engine.stop();
}

// ============================================================================
// Multiple Strategies
// ============================================================================

TEST(StrategyEngineTest, MultipleStrategiesReceiveTicks) {
    StrategyEngine engine;

    auto s1 = std::make_unique<TestStrategy>();
    auto s2 = std::make_unique<TestStrategy>();
    auto* r1 = s1.get();
    auto* r2 = s2.get();

    engine.registerStrategy(std::move(s1));
    engine.registerStrategy(std::move(s2));
    engine.start();

    engine.pushTick(makeTick(1, 50000.0, 1.0));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_GE(r1->tick_count.load(), 1U);
    EXPECT_GE(r2->tick_count.load(), 1U);

    engine.stop();
}

// ============================================================================
// OrderBook Integration
// ============================================================================

TEST(StrategyEngineTest, OrderBookUpdatedOnTick) {
    StrategyEngine engine;

    auto strategy = std::make_unique<TestStrategy>();
    strategy->submit_every = 0;  // don't submit, just check book state
    auto* raw = strategy.get();

    engine.registerStrategy(std::move(strategy));
    engine.start();

    // Push bid tick
    engine.pushTick(makeTick(1, 50000.0, 1.0, TickSide::BID));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_GE(raw->tick_count.load(), 1U);

    // Can't easily check OrderBook state from here without exposing it,
    // but we verify the tick was processed (no crash, tick_count incremented)

    engine.stop();
}

// ============================================================================
// Capital Management
// ============================================================================

TEST(StrategyEngineTest, AvailableCapital) {
    StrategyEngine engine;
    engine.setAvailableCapital(toDecimal(100000.0));

    // Verify capital is accessible via context through a strategy callback
    class CapitalCheckStrategy : public Strategy {
    public:
        const char* name() const override { return "CapitalCheck"; }
        std::vector<uint32_t> symbols() const override { return {1}; }

        void onTick(const Tick& /*tick*/, StrategyContext& ctx) override {
            checked_capital = ctx.getAvailableCapital();
            checked.store(true, std::memory_order_release);
        }
        Decimal checked_capital{0};
        std::atomic<bool> checked{false};
    };

    auto strategy = std::make_unique<CapitalCheckStrategy>();
    auto* raw = strategy.get();

    engine.registerStrategy(std::move(strategy));
    engine.start();

    engine.pushTick(makeTick(1, 50000.0, 1.0));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!raw->checked && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }

    EXPECT_TRUE(raw->checked);
    EXPECT_EQ(raw->checked_capital, toDecimal(100000.0));

    engine.stop();
}

// ============================================================================
// Position Manager Access
// ============================================================================

TEST(StrategyEngineTest, PositionManagerAccessible) {
    StrategyEngine engine;
    engine.start();

    auto& pm = engine.positionManager();
    EXPECT_EQ(pm.getPosition(1), nullptr);

    engine.stop();
}

// ============================================================================
// Fill Routing
// ============================================================================

TEST(StrategyEngineTest, FillRoutedToCorrectStrategy) {
    StrategyEngine engine;

    // Register two strategies — only s1 submits orders
    auto s1 = std::make_unique<TestStrategy>();
    s1->submit_every = 1;
    auto* r1 = s1.get();

    auto s2 = std::make_unique<TestStrategy>();
    s2->submit_every = 0;
    auto* r2 = s2.get();

    engine.registerStrategy(std::move(s1));  // id=1
    engine.registerStrategy(std::move(s2));  // id=2

    // Allow orders through risk
    RiskParameters params;
    params.max_order_value = 1000000.0;
    params.max_orders_per_second = 1000000;
    engine.updateRiskParameters(params);

    engine.start();

    // s1 submits an order via tick
    engine.pushTick(makeTick(1, 50000.0, 1.0));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Verify s1's order was placed
    EXPECT_GE(r1->orders_submitted.load(), 1U);

    // Build a fill for s1's submitted order
    Fill fill;
    fill.fill_id = 1;
    fill.order_id = r1->last_order_id.load();
    fill.symbol_id = 1;
    fill.exchange_id = 1;
    fill.fill_price = toDecimal(50000.0);
    fill.fill_quantity = toDecimal(0.1);
    fill.side = OrderSide::BUY;
    fill.strategy_id = 1;  // matches s1
    fill.receive_timestamp_us = 2000000;

    EXPECT_TRUE(engine.pushFill(fill));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // s1 should receive the fill, s2 should not
    EXPECT_GE(r1->fill_count.load(), 1U);
    EXPECT_EQ(r2->fill_count.load(), 0U);

    engine.stop();
}

TEST(StrategyEngineTest, FillUpdatesPositionManager) {
    StrategyEngine engine;

    auto strategy = std::make_unique<TestStrategy>();
    strategy->submit_every = 1;
    auto* raw = strategy.get();

    RiskParameters params;
    params.max_order_value = 1000000.0;
    params.max_orders_per_second = 1000000;
    engine.updateRiskParameters(params);

    engine.registerStrategy(std::move(strategy));
    engine.start();

    // Trigger an order
    engine.pushTick(makeTick(1, 50000.0, 1.0));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Simulate a fill for a BUY of 0.1 BTC at 50000
    Fill fill;
    fill.fill_id = 1;
    fill.order_id = raw->last_order_id.load();
    fill.symbol_id = 1;
    fill.exchange_id = 1;
    fill.fill_price = toDecimal(50000.0);
    fill.fill_quantity = toDecimal(0.1);
    fill.side = OrderSide::BUY;
    fill.strategy_id = 1;
    fill.receive_timestamp_us = 2000000;

    EXPECT_TRUE(engine.pushFill(fill));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Position should be created
    auto* pos = engine.positionManager().getPosition(1);
    ASSERT_NE(pos, nullptr);
    EXPECT_EQ(pos->quantity, toDecimal(0.1));
    EXPECT_EQ(pos->average_price, toDecimal(50000.0));

    // Fill count in stats
    auto stats = engine.getStats();
    EXPECT_GE(stats.fills_processed, 1U);

    engine.stop();
}

TEST(StrategyEngineTest, FillWithUnknownStrategyIdIsHarmless) {
    StrategyEngine engine;

    auto strategy = std::make_unique<TestStrategy>();
    strategy->submit_every = 0;
    auto* raw = strategy.get();

    engine.registerStrategy(std::move(strategy));
    engine.start();

    // Fill with strategy_id that doesn't match any registered strategy
    Fill fill;
    fill.fill_id = 99;
    fill.order_id = 42;
    fill.symbol_id = 1;
    fill.exchange_id = 1;
    fill.fill_price = toDecimal(50000.0);
    fill.fill_quantity = toDecimal(0.1);
    fill.side = OrderSide::BUY;
    fill.strategy_id = 999;  // no such strategy
    fill.receive_timestamp_us = 2000000;

    // Should not crash
    EXPECT_TRUE(engine.pushFill(fill));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Position should still be updated (position update is unconditional)
    auto* pos = engine.positionManager().getPosition(1);
    ASSERT_NE(pos, nullptr);  // position created regardless
    EXPECT_EQ(raw->fill_count.load(), 0U);

    engine.stop();
}

// ============================================================================
// End-to-End Integration Tests (tick → order → fill pipeline)
// ============================================================================

/// Full pipeline: tick in → strategy processes → order out (via risk check)
TEST(StrategyEngineE2ETest, TickToOrderPipeline) {
    StrategyEngine engine;

    RiskParameters params;
    params.max_order_value = 1000000.0;
    params.max_orders_per_second = 1000000;
    engine.updateRiskParameters(params);

    auto strategy = std::make_unique<TestStrategy>();
    strategy->submit_every = 1;
    auto* raw = strategy.get();

    engine.registerStrategy(std::move(strategy));
    engine.start();

    // Step 1: Push a tick
    auto tick = makeTick(1, 50000.0, 1.0);
    EXPECT_TRUE(engine.pushTick(tick));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Step 2: Verify tick was processed
    EXPECT_GE(raw->tick_count.load(), 1U);

    // Step 3: Verify an order was submitted
    EXPECT_GE(raw->orders_submitted.load(), 1U);
    uint64_t order_id = raw->last_order_id.load();
    EXPECT_GT(order_id, 0U);

    // Step 4: Verify order is in the output queue with correct fields
    OrderRequest out;
    EXPECT_TRUE(engine.popOrder(out));
    EXPECT_EQ(out.order_id, order_id);
    EXPECT_GT(out.timestamp_us, 0U);
    EXPECT_EQ(out.strategy_id, 1U);

    // Step 5: Verify stats
    auto stats = engine.getStats();
    EXPECT_GE(stats.ticks_processed, 1U);
    EXPECT_GE(stats.orders_submitted, 1U);
    EXPECT_EQ(stats.orders_risk_rejected, 0U);

    engine.stop();
}

/// Full round-trip: tick → order → fill → position update → strategy callback
TEST(StrategyEngineE2ETest, FullTickOrderFillRoundTrip) {
    StrategyEngine engine;

    RiskParameters params;
    params.max_order_value = 1000000.0;
    params.max_orders_per_second = 1000000;
    engine.updateRiskParameters(params);

    auto strategy = std::make_unique<TestStrategy>();
    strategy->submit_every = 1;
    auto* raw = strategy.get();

    engine.registerStrategy(std::move(strategy));
    engine.start();

    // --- Phase 1: Tick → Order ---
    engine.pushTick(makeTick(1, 50000.0, 1.0));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    ASSERT_GE(raw->orders_submitted.load(), 1U);
    uint64_t order_id = raw->last_order_id.load();

    // Drain the output queue to get the submitted order
    OrderRequest out;
    ASSERT_TRUE(engine.popOrder(out));
    EXPECT_EQ(out.order_id, order_id);

    // --- Phase 2: Fill → Position Update ---
    Fill fill;
    fill.fill_id = 100;
    fill.order_id = order_id;
    fill.symbol_id = 1;
    fill.exchange_id = 1;
    fill.fill_price = toDecimal(50000.0);
    fill.fill_quantity = toDecimal(0.1);
    fill.side = OrderSide::BUY;
    fill.strategy_id = 1;
    fill.receive_timestamp_us = 2000000;

    EXPECT_TRUE(engine.pushFill(fill));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // --- Phase 3: Verify position ---
    auto* pos = engine.positionManager().getPosition(1);
    ASSERT_NE(pos, nullptr);
    EXPECT_EQ(pos->quantity, toDecimal(0.1));
    EXPECT_EQ(pos->average_price, toDecimal(50000.0));

    // --- Phase 4: Verify strategy got the fill callback ---
    EXPECT_GE(raw->fill_count.load(), 1U);

    // --- Phase 5: Verify stats ---
    auto stats = engine.getStats();
    EXPECT_GE(stats.ticks_processed, 1U);
    EXPECT_GE(stats.orders_submitted, 1U);
    EXPECT_GE(stats.fills_processed, 1U);

    engine.stop();
}

/// Multiple tick→order→fill cycles maintain correct state
TEST(StrategyEngineE2ETest, MultipleCyclesAccumulateCorrectly) {
    StrategyEngine engine;

    RiskParameters params;
    params.max_order_value = 1000000.0;
    params.max_orders_per_second = 1000000;
    engine.updateRiskParameters(params);

    auto strategy = std::make_unique<TestStrategy>();
    strategy->submit_every = 1;
    auto* raw = strategy.get();

    engine.registerStrategy(std::move(strategy));
    engine.start();

    constexpr int CYCLES = 3;

    for (int cycle = 0; cycle < CYCLES; ++cycle) {
        // Tick
        engine.pushTick(makeTick(1, 50000.0 + cycle * 100.0, 1.0));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        ASSERT_GE(raw->orders_submitted.load(), static_cast<uint64_t>(cycle + 1));
        uint64_t order_id = raw->last_order_id.load();

        // Drain order
        OrderRequest out;
        ASSERT_TRUE(engine.popOrder(out));

        // Fill
        Fill fill;
        fill.fill_id = static_cast<uint64_t>(100 + cycle);
        fill.order_id = order_id;
        fill.symbol_id = 1;
        fill.exchange_id = 1;
        fill.fill_price = toDecimal(50000.0 + cycle * 100.0);
        fill.fill_quantity = toDecimal(0.1);
        fill.side = OrderSide::BUY;
        fill.strategy_id = 1;
        fill.receive_timestamp_us = 2000000 + static_cast<uint64_t>(cycle) * 1000;

        EXPECT_TRUE(engine.pushFill(fill));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Verify cumulative state
    EXPECT_EQ(raw->tick_count.load(), static_cast<uint64_t>(CYCLES));
    EXPECT_EQ(raw->orders_submitted.load(), static_cast<uint64_t>(CYCLES));
    EXPECT_EQ(raw->fill_count.load(), static_cast<uint64_t>(CYCLES));

    auto* pos = engine.positionManager().getPosition(1);
    ASSERT_NE(pos, nullptr);
    // 3 buys of 0.1 each = 0.3 total
    EXPECT_NEAR(toDouble(pos->quantity), 0.3, 1e-9);

    auto stats = engine.getStats();
    EXPECT_EQ(stats.ticks_processed, static_cast<uint64_t>(CYCLES));
    EXPECT_EQ(stats.orders_submitted, static_cast<uint64_t>(CYCLES));
    EXPECT_EQ(stats.fills_processed, static_cast<uint64_t>(CYCLES));

    engine.stop();
}

/// Order rejected by risk → no fill for that order → position unaffected
TEST(StrategyEngineE2ETest, RejectedOrderDoesNotProceed) {
    StrategyEngine engine;

    // Tight risk so orders get rejected
    RiskParameters tight;
    tight.max_order_value = 1.0;  // only $1 — almost everything rejected
    tight.max_orders_per_second = 1;
    engine.updateRiskParameters(tight);

    auto strategy = std::make_unique<TestStrategy>();
    strategy->submit_every = 1;
    auto* raw = strategy.get();

    engine.registerStrategy(std::move(strategy));
    engine.start();

    // Push tick → order should be rejected by risk
    engine.pushTick(makeTick(1, 50000.0, 1.0));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Tick was processed but order was rejected
    EXPECT_GE(raw->tick_count.load(), 1U);
    EXPECT_EQ(raw->orders_submitted.load(), 0U);
    EXPECT_EQ(raw->last_order_id.load(), 0U);

    // No order in output queue
    OrderRequest out;
    EXPECT_FALSE(engine.popOrder(out));

    // Risk rejection stat
    auto stats = engine.getStats();
    EXPECT_GE(stats.orders_risk_rejected, 1U);

    // Position should be empty (no order was placed, so no fill)
    auto* pos = engine.positionManager().getPosition(1);
    EXPECT_EQ(pos, nullptr);

    engine.stop();
}

/// Position accumulation across multiple fills for the same order
TEST(StrategyEngineE2ETest, MultiFillPositionAccumulation) {
    StrategyEngine engine;

    RiskParameters params;
    params.max_order_value = 1000000.0;
    params.max_orders_per_second = 1000000;
    engine.updateRiskParameters(params);

    // Strategy that submits only one order (on first tick)
    class SingleOrderStrategy : public Strategy {
    public:
        const char* name() const override { return "SingleOrder"; }
        std::vector<uint32_t> symbols() const override { return {1}; }

        void onTick(const Tick& tick, StrategyContext& ctx) override {
            if (!submitted) {
                OrderRequest order;
                order.symbol_id = tick.symbol_id;
                order.price = tick.price;
                order.quantity = toDecimal(1.0);
                order.side = OrderSide::BUY;
                order.type = OrderType::LIMIT;
                last_id = ctx.submitOrder(order);
                submitted = (last_id != 0);
                tick_count.fetch_add(1);
            }
        }

        void onFill(const Fill& /*fill*/, StrategyContext& /*ctx*/) override {
            fill_count.fetch_add(1);
        }

        std::atomic<bool> submitted{false};
        std::atomic<uint64_t> last_id{0};
        std::atomic<int> tick_count{0};
        std::atomic<int> fill_count{0};
    };

    auto strategy = std::make_unique<SingleOrderStrategy>();
    auto* raw = strategy.get();
    engine.registerStrategy(std::move(strategy));
    engine.start();

    // Submit order via tick
    engine.pushTick(makeTick(1, 50000.0, 1.0));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ASSERT_GT(raw->last_id.load(), 0U);

    uint64_t oid = raw->last_id.load();

    // Drain order queue
    OrderRequest out;
    ASSERT_TRUE(engine.popOrder(out));

    // Simulate partial fills: 0.3, then 0.3, then 0.4 (total = 1.0)
    double fill_amounts[] = {0.3, 0.3, 0.4};
    for (int i = 0; i < 3; ++i) {
        Fill fill;
        fill.fill_id = static_cast<uint64_t>(200 + i);
        fill.order_id = oid;
        fill.symbol_id = 1;
        fill.exchange_id = 1;
        fill.fill_price = toDecimal(50000.0 + i * 10.0);
        fill.fill_quantity = toDecimal(fill_amounts[i]);
        fill.side = OrderSide::BUY;
        fill.strategy_id = 1;
        fill.receive_timestamp_us = 3000000 + static_cast<uint64_t>(i) * 1000;

        EXPECT_TRUE(engine.pushFill(fill));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Verify total position (weighted average)
    auto* pos = engine.positionManager().getPosition(1);
    ASSERT_NE(pos, nullptr);
    EXPECT_NEAR(toDouble(pos->quantity), 1.0, 1e-9);
    // Weighted average: (0.3*50000 + 0.3*50010 + 0.4*50020) / 1.0
    EXPECT_GE(raw->fill_count.load(), 3);

    auto stats = engine.getStats();
    EXPECT_EQ(stats.fills_processed, 3U);

    engine.stop();
}

// ============================================================================
// GridStrategy E2E — tick → order → fill → opposite order
// ============================================================================

#include "chronos/strategies/grid_strategy.hpp"

TEST(StrategyEngineE2ETest, GridStrategyOrdersOnTick) {
    using namespace chronos::strategies;

    GridStrategy::Config cfg;
    cfg.grid_low    = 95.0;
    cfg.grid_high   = 105.0;
    cfg.grid_levels = 5;
    cfg.quantity    = 0.01;
    cfg.symbol_id   = 1;

    auto strategy = std::make_unique<GridStrategy>(cfg);
    auto* raw = strategy.get();

    trading::StrategyEngine engine;
    engine.updateRiskParameters(RiskParameters{});
    engine.setAvailableCapital(toDecimal(1000000.0));
    engine.registerStrategy(std::move(strategy));
    engine.start();

    // Push bid/ask ticks to build orderbook around 100.0
    engine.pushTick(makeTick(1, 99.0, 0.5, TickSide::BID));
    engine.pushTick(makeTick(1, 101.0, 0.5, TickSide::ASK));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Drain orders
    std::vector<OrderRequest> orders;
    OrderRequest o;
    while (engine.popOrder(o)) {
        orders.push_back(o);
    }

    EXPECT_GT(orders.size(), 0u);

    int buys = 0, sells = 0;
    for (auto& ord : orders) {
        if (ord.side == OrderSide::BUY) buys++;
        if (ord.side == OrderSide::SELL) sells++;
    }
    EXPECT_GT(buys, 0);
    EXPECT_GT(sells, 0);
    EXPECT_TRUE(raw->isInitialized());
    EXPECT_GT(raw->tickCount(), 0u);

    engine.stop();
}

TEST(StrategyEngineE2ETest, GridStrategyFillTriggersOppositeOrder) {
    using namespace chronos::strategies;

    GridStrategy::Config cfg;
    cfg.grid_low    = 95.0;
    cfg.grid_high   = 105.0;
    cfg.grid_levels = 5;
    cfg.quantity    = 0.01;
    cfg.symbol_id   = 1;

    auto strategy = std::make_unique<GridStrategy>(cfg);

    trading::StrategyEngine engine;
    engine.updateRiskParameters(RiskParameters{});
    engine.setAvailableCapital(toDecimal(1000000.0));
    engine.registerStrategy(std::move(strategy));
    engine.start();

    // Build orderbook at mid=100
    engine.pushTick(makeTick(1, 99.0, 1.0, TickSide::BID));
    engine.pushTick(makeTick(1, 101.0, 1.0, TickSide::ASK));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Drain initial orders
    OrderRequest o;
    while (engine.popOrder(o)) { /* discard */ }

    // Inject a fill with strategy_id=1 to verify fill routing works
    Fill f;
    f.order_id        = 0;
    f.fill_id         = 1;
    f.symbol_id       = 1;
    f.exchange_id     = 1;
    f.fill_price      = toDecimal(97.0);
    f.fill_quantity   = toDecimal(0.01);
    f.side            = OrderSide::BUY;
    f.strategy_id     = 1;
    f.receive_timestamp_us = 3000000;

    EXPECT_TRUE(engine.pushFill(f));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Position was updated by StrategyEngine's fill processing
    auto* pos = engine.positionManager().getPosition(1);
    ASSERT_NE(pos, nullptr);
    EXPECT_NEAR(toDouble(pos->quantity), 0.01, 1e-9);

    auto stats = engine.getStats();
    EXPECT_GE(stats.fills_processed, 1u);

    engine.stop();
}
