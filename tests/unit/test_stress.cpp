/**
 * @file test_stress.cpp
 * @brief Stress tests — sustained load, queue overflow, rapid lifecycle, multi-strategy
 *
 * Validates: system stability under load, no memory leaks, no deadlocks,
 *           graceful degradation under overload.
 */

#include <gtest/gtest.h>
#include <chronos/trading/strategy_engine.hpp>
#include <chronos/core/types.hpp>
#include <thread>
#include <atomic>
#include <vector>

using namespace chronos;
using namespace chronos::trading;

namespace {

Tick makeTick(uint32_t symbol_id, double price, TickSide side = TickSide::BID) {
    Tick t;
    t.symbol_id = symbol_id;
    t.price = toDecimal(price);
    t.quantity = toDecimal(1.0);
    t.side = side;
    t.receive_timestamp_us = 1000000;
    return t;
}

/// Strategy that submits an order on every N-th tick for sustained load testing.
/// Spacing out submissions prevents instant output queue overflow (capacity 1024).
class LoadStrategy : public Strategy {
public:
    explicit LoadStrategy(size_t submit_every = 1) : submit_every_(submit_every) {}

    const char* name() const override { return "LoadStrategy"; }
    std::vector<uint32_t> symbols() const override { return {}; }

    void onTick(const Tick& tick, StrategyContext& ctx) override {
        uint64_t n = ticks_processed.fetch_add(1, std::memory_order_relaxed) + 1;

        if (submit_every_ > 0 && (n % submit_every_) == 0) {
            OrderRequest order;
            order.symbol_id = tick.symbol_id;
            order.price      = tick.price;
            order.quantity   = toDecimal(0.01);
            order.side       = OrderSide::BUY;
            order.type       = OrderType::LIMIT;

            uint64_t id = ctx.submitOrder(order);
            if (id) orders_submitted.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void onFill(const Fill& /*fill*/, StrategyContext& /*ctx*/) override {
        fill_count.fetch_add(1, std::memory_order_relaxed);
    }

    std::atomic<uint64_t> ticks_processed{0};
    std::atomic<uint64_t> orders_submitted{0};
    std::atomic<uint64_t> fill_count{0};

private:
    size_t submit_every_;
};

/// Drain orders from the engine output queue, optionally simulating fills.
size_t drainOrders(StrategyEngine& engine, bool simulate_fills = false) {
    size_t count = 0;
    OrderRequest out;
    while (engine.popOrder(out)) {
        if (out.quantity == Decimal(0)) continue; // skip cancels
        count++;

        if (simulate_fills) {
            Fill fill;
            fill.order_id = out.order_id;
            fill.symbol_id = out.symbol_id;
            fill.fill_price = out.price;
            fill.fill_quantity = out.quantity;
            fill.side = out.side;
            fill.strategy_id = out.strategy_id;
            fill.receive_timestamp_us = out.timestamp_us;
            engine.pushFill(fill);
        }
    }
    return count;
}

} // anonymous namespace

// ============================================================================
// 1. Sustained Load — 100K ticks through the full pipeline
// ============================================================================

TEST(StressTest, SustainedLoad_100k_Ticks) {
    constexpr size_t TOTAL_TICKS = 100000;
    constexpr size_t BURST_SIZE = 50;  // small bursts to avoid queue overflow

    StrategyEngine engine;
    engine.setAvailableCapital(toDecimal(1000000.0));

    RiskParameters params;
    params.max_order_value = 1000000.0;
    params.max_orders_per_second = 10000000;
    engine.updateRiskParameters(params);

    auto strategy = std::make_unique<LoadStrategy>(1);  // order on every tick
    auto* raw = strategy.get();
    engine.registerStrategy(std::move(strategy));
    engine.start();

    size_t pushed = 0;
    size_t total_drained = 0;
    size_t total_fills = 0;

    while (pushed < TOTAL_TICKS) {
        for (size_t i = 0; i < BURST_SIZE && pushed < TOTAL_TICKS; ++i) {
            Tick tick;
            tick.symbol_id = 1;
            tick.price = toDecimal(50000.0 + (pushed % 1000) * 0.005);
            tick.quantity = toDecimal(1.0);
            tick.side = (pushed % 2 == 0) ? TickSide::BID : TickSide::ASK;
            tick.receive_timestamp_us = 1000000 + pushed;

            while (!engine.pushTick(tick)) {
                // Tick queue full — drain output and yield
                total_drained += drainOrders(engine, true);
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
            pushed++;
        }

        // Drain after each burst to prevent output queue overflow
        total_drained += drainOrders(engine, true);
        total_fills += total_drained;  // approximate
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    // Final drain
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    total_drained += drainOrders(engine, false);

    engine.stop();

    // Verify no data loss in tick processing
    EXPECT_EQ(raw->ticks_processed.load(), TOTAL_TICKS)
        << "All " << TOTAL_TICKS << " ticks should be processed";

    auto stats = engine.getStats();
    EXPECT_EQ(stats.ticks_processed, TOTAL_TICKS);
    EXPECT_GT(stats.orders_submitted, 0u)
        << "At least some orders should have been submitted";
    EXPECT_EQ(stats.orders_queue_dropped, 0u)
        << "No orders should be dropped with proper draining";
    EXPECT_GT(total_drained, 0u)
        << "At least some orders should have been drained";

    (void)total_fills;
}

// ============================================================================
// 2. Queue Overflow — verify no crash, no data corruption under overload
// ============================================================================

TEST(StressTest, QueueOverflowGracefulDegradation) {
    StrategyEngine engine;
    engine.setAvailableCapital(toDecimal(100000.0));

    RiskParameters params;
    params.max_order_value = 1000000.0;
    params.max_orders_per_second = 10000000;
    engine.updateRiskParameters(params);

    auto strategy = std::make_unique<LoadStrategy>(1);
    engine.registerStrategy(std::move(strategy));
    engine.start();

    // Push ticks as fast as possible without draining
    size_t pushed = 0;
    size_t rejected = 0;
    for (size_t i = 0; i < 5000; ++i) {
        Tick tick;
        tick.symbol_id = 2;
        tick.price = toDecimal(50000.0 + i * 0.001);
        tick.quantity = toDecimal(1.0);
        tick.side = TickSide::BID;
        tick.receive_timestamp_us = 1000000 + i;

        if (engine.pushTick(tick)) {
            pushed++;
        } else {
            rejected++;
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    }

    // Drain everything
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    drainOrders(engine, false);

    engine.stop();

    // Queue overflow should NOT cause crashes or data corruption
    auto stats = engine.getStats();
    EXPECT_EQ(stats.ticks_processed, pushed)
        << "All pushed ticks must be processed";
    EXPECT_GT(rejected, 0u)
        << "Some ticks should be rejected when queue is full (validating overflow path)";
}

// ============================================================================
// 3. Rapid Start/Stop Cycles — verify no memory leaks, no deadlocks
// ============================================================================

TEST(StressTest, RapidStartStopCycles) {
    constexpr int CYCLES = 20;

    for (int i = 0; i < CYCLES; ++i) {
        StrategyEngine engine;
        engine.setAvailableCapital(toDecimal(100000.0));

        RiskParameters params;
        params.max_order_value = 1000000.0;
        engine.updateRiskParameters(params);

        auto strategy = std::make_unique<LoadStrategy>(1);
        engine.registerStrategy(std::move(strategy));
        engine.start();

        engine.pushTick(makeTick(1, 50000.0));
        engine.pushTick(makeTick(1, 50001.0));

        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        engine.stop();
    }

    SUCCEED() << "Completed " << CYCLES << " start/stop cycles without issues";
}

// ============================================================================
// 4. Multiple Strategies — concurrent execution, no interference
// ============================================================================

TEST(StressTest, MultipleStrategiesConcurrently) {
    constexpr int NUM_STRATEGIES = 4;
    constexpr size_t TICKS_PER_SYMBOL = 200;
    constexpr size_t BURST_SIZE = 20;

    StrategyEngine engine;
    engine.setAvailableCapital(toDecimal(1000000.0));

    RiskParameters params;
    params.max_order_value = 1000000.0;
    params.max_orders_per_second = 10000000;
    engine.updateRiskParameters(params);

    std::vector<LoadStrategy*> strategies;
    for (int i = 0; i < NUM_STRATEGIES; ++i) {
        auto s = std::make_unique<LoadStrategy>(2);  // order every 2nd tick
        auto* raw = s.get();
        strategies.push_back(raw);
        engine.registerStrategy(std::move(s));
    }

    engine.start();

    size_t total_pushed = 0;
    for (int sym = 1; sym <= NUM_STRATEGIES; ++sym) {
        for (size_t i = 0; i < TICKS_PER_SYMBOL; i += BURST_SIZE) {
            for (size_t j = 0; j < BURST_SIZE && (i + j) < TICKS_PER_SYMBOL; ++j) {
                Tick tick;
                tick.symbol_id = static_cast<uint32_t>(sym);
                tick.price = toDecimal(50000.0 + sym * 100.0 + (i + j) * 0.01);
                tick.quantity = toDecimal(1.0);
                tick.side = ((i + j) % 2 == 0) ? TickSide::BID : TickSide::ASK;
                tick.receive_timestamp_us = 1000000 + i + j;

                while (!engine.pushTick(tick)) {
                    drainOrders(engine, false);
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                }
                total_pushed++;
            }

            drainOrders(engine, false);
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    // Final drain
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    drainOrders(engine, false);

    engine.stop();

    auto stats = engine.getStats();
    EXPECT_EQ(stats.ticks_processed, total_pushed);

    uint64_t total_ticks_processed = 0;
    for (auto* s : strategies) {
        total_ticks_processed += s->ticks_processed.load();
    }

    // Each tick dispatched to all wildcard strategies
    EXPECT_EQ(total_ticks_processed, total_pushed * NUM_STRATEGIES)
        << "Each tick should be dispatched to all wildcard strategies";
    EXPECT_GT(total_ticks_processed, 0u);
}

// ============================================================================
// 5. Position Manager Stress — many fills across multiple symbols
// ============================================================================

TEST(StressTest, PositionManagerManyFills) {
    constexpr size_t NUM_FILLS = 20000;
    constexpr size_t BURST_SIZE = 50;
    constexpr int NUM_SYMBOLS = 20;

    StrategyEngine engine;
    engine.setAvailableCapital(toDecimal(10000000.0));

    RiskParameters params;
    params.max_order_value = 10000000.0;
    params.max_orders_per_second = 10000000;
    engine.updateRiskParameters(params);

    auto strategy = std::make_unique<LoadStrategy>(1);
    auto* raw = strategy.get();
    engine.registerStrategy(std::move(strategy));
    engine.start();

    size_t pushed = 0;
    for (size_t i = 0; i < NUM_FILLS; i += BURST_SIZE) {
        for (size_t j = 0; j < BURST_SIZE && (i + j) < NUM_FILLS; ++j) {
            Fill fill;
            fill.fill_id = static_cast<uint64_t>(i + j + 1);
            fill.order_id = static_cast<uint64_t>(i + j + 1000);
            fill.symbol_id = static_cast<uint32_t>(((i + j) % NUM_SYMBOLS) + 1);
            fill.fill_price = toDecimal(50000.0 + ((i + j) % 1000) * 0.01);
            fill.fill_quantity = toDecimal(((i + j) % 2 == 0) ? 0.1 : -0.1);
            fill.side = ((i + j) % 2 == 0) ? OrderSide::BUY : OrderSide::SELL;
            fill.strategy_id = 1;
            fill.receive_timestamp_us = 1000000 + i + j;

            while (!engine.pushFill(fill)) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            pushed++;
        }

        // Let engine process the burst
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // Verify positions
    size_t positions_with_quantity = 0;
    for (int sym = 1; sym <= NUM_SYMBOLS; ++sym) {
        auto* pos = engine.positionManager().getPosition(static_cast<uint32_t>(sym));
        if (pos && pos->quantity != Decimal(0)) {
            positions_with_quantity++;
        }
    }

    engine.stop();

    auto stats = engine.getStats();
    EXPECT_EQ(stats.fills_processed, pushed)
        << "All pushed fills must be processed";
    EXPECT_GT(positions_with_quantity, 0u)
        << "At least some symbols should have non-zero positions";
    EXPECT_GT(raw->fill_count.load(), 0u)
        << "Strategy should have received fill callbacks";
}
