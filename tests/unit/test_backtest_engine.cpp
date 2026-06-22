#include <gtest/gtest.h>
#include <filesystem>
#include <thread>

#include "chronos/backtest/backtest_engine.hpp"
#include "chronos/core/types.hpp"
#include "chronos/logging/log_reader.hpp"
#include "chronos/logging/log_writer.hpp"

using namespace chronos;
using namespace chronos::backtest;
using namespace chronos::logging;

namespace fs = std::filesystem;

// ============================================================================
// Helpers
// ============================================================================

namespace {

const std::string TEST_DIR = "/tmp/chronos_test_backtest";

void cleanDir() {
    std::error_code ec;
    if (fs::exists(TEST_DIR, ec)) {
        fs::remove_all(TEST_DIR, ec);
    }
}

// Write tick data to a temp directory, return path to the tick file
std::string writeTicks(const std::string& dir,
                        const std::vector<std::tuple<double, double, TickSide, uint64_t>>& ticks) {
    LogWriter writer;
    LogConfig cfg;
    cfg.log_dir = dir;
    cfg.buffer_size = 65536;
    cfg.flush_interval_ms = 10;
    writer.initialize(dir, cfg);

    for (auto& [price, qty, side, ts] : ticks) {
        Tick t;
        t.price = toDecimal(price);
        t.quantity = toDecimal(qty);
        t.side = side;
        t.symbol_id = 1;
        t.exchange_timestamp_us = ts;
        writer.writeTick(t);
    }
    writer.flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    writer.stop();

    for (auto& entry : fs::directory_iterator(dir)) {
        std::string name = entry.path().filename().string();
        if (name.find("tick_") != std::string::npos) {
            return entry.path().string();
        }
    }
    return "";
}

// ============================================================================
// Test Strategy — records callbacks, optionally submits orders
// ============================================================================

struct TestStrategy : public trading::Strategy {
    int tick_count = 0;
    int fill_count = 0;
    int onload_called = 0;
    std::vector<Tick> ticks;
    std::vector<Fill> fills;

    // Configure submission behaviour
    bool auto_submit = false;
    int submit_on_tick_n = 1;     // submit on Nth tick
    OrderSide submit_side = OrderSide::BUY;
    double submit_price = 100.0;
    double submit_qty = 0.1;

    const char* name() const override { return "TestStrategy"; }

    void onLoad(trading::StrategyContext& /*ctx*/) override {
        onload_called++;
    }

    void onTick(const Tick& tick, trading::StrategyContext& ctx) override {
        tick_count++;
        ticks.push_back(tick);

        if (auto_submit && tick_count == submit_on_tick_n) {
            OrderRequest order;
            order.symbol_id = tick.symbol_id;
            order.side = submit_side;
            order.price = toDecimal(submit_price);
            order.quantity = toDecimal(submit_qty);
            order.type = OrderType::LIMIT;
            ctx.submitOrder(order);
        }
    }

    void onFill(const Fill& fill, trading::StrategyContext& /*ctx*/) override {
        fill_count++;
        fills.push_back(fill);
    }
};

}  // namespace

// ============================================================================
// Test Fixture — cleans up temp dir
// ============================================================================

class BacktestEngineTest : public ::testing::Test {
protected:
    void SetUp() override   { cleanDir(); fs::create_directories(TEST_DIR); }
    void TearDown() override { cleanDir(); }
};

// ============================================================================
// 1. Lifecycle
// ============================================================================

TEST_F(BacktestEngineTest, DefaultConstruction) {
    BacktestEngine engine;
    EXPECT_NE(engine.metrics().totalTrades(), 1000u);  // just checks accessible
}

TEST_F(BacktestEngineTest, ConfigDefaults) {
    BacktestEngine engine;
    EXPECT_EQ(engine.config().fill_mode, BacktestConfig::NEXT_TICK);
    EXPECT_EQ(engine.config().initial_capital, Decimal{0});
    EXPECT_DOUBLE_EQ(engine.config().taker_fee, 0.0);
    EXPECT_TRUE(engine.config().record_equity_every_tick);
}

TEST_F(BacktestEngineTest, ModifyConfig) {
    BacktestEngine engine;
    engine.config().fill_mode = BacktestConfig::IMMEDIATE;
    engine.config().initial_capital = toDecimal(50000.0);
    engine.config().taker_fee = 0.001;

    EXPECT_EQ(engine.config().fill_mode, BacktestConfig::IMMEDIATE);
    EXPECT_EQ(toDouble(engine.config().initial_capital), 50000.0);
    EXPECT_DOUBLE_EQ(engine.config().taker_fee, 0.001);
}

TEST_F(BacktestEngineTest, SetStrategy) {
    BacktestEngine engine;
    auto strat = std::make_unique<TestStrategy>();
    engine.setStrategy(std::move(strat));
    // No crash, strategy accepted
}

// ============================================================================
// 2. Run without strategy
// ============================================================================

TEST_F(BacktestEngineTest, RunWithoutStrategy) {
    BacktestEngine engine;

    auto tick_path = writeTicks(TEST_DIR, {
        {100.0, 1.0, TickSide::BID, 1000},
    });
    ASSERT_FALSE(tick_path.empty());

    LogReader reader;
    ASSERT_TRUE(reader.open(tick_path));

    LogFileSet logSet;
    logSet.tick = std::move(reader);
    engine.setData(logSet);

    engine.run();  // returns immediately, no crash
    EXPECT_EQ(engine.metrics().totalTrades(), 0u);
}

// ============================================================================
// 3. Run with ticks — basic integration
// ============================================================================

TEST_F(BacktestEngineTest, RunDeliversTicksToStrategy) {
    auto strat_ptr = new TestStrategy();
    auto strat = std::unique_ptr<TestStrategy>(strat_ptr);

    auto tick_path = writeTicks(TEST_DIR, {
        {100.0, 1.0, TickSide::BID, 1000},
        {101.0, 1.0, TickSide::ASK, 2000},
        {100.5, 1.0, TickSide::BID, 3000},
    });
    ASSERT_FALSE(tick_path.empty());

    LogReader reader;
    ASSERT_TRUE(reader.open(tick_path));

    BacktestEngine engine;
    engine.setStrategy(std::move(strat));

    LogFileSet logSet;
    logSet.tick = std::move(reader);
    engine.setData(logSet);

    engine.run();

    EXPECT_EQ(strat_ptr->tick_count, 3);
    EXPECT_EQ(strat_ptr->onload_called, 1);
}

// ============================================================================
// 4. Order submission and fill
// ============================================================================

TEST_F(BacktestEngineTest, OrderSubmissionAndFill) {
    auto strat_ptr = new TestStrategy();
    auto strat = std::unique_ptr<TestStrategy>(strat_ptr);
    strat_ptr->auto_submit = true;
    strat_ptr->submit_on_tick_n = 2;  // submit on 2nd tick

    // First tick sets up bid, second sets up ask + triggers order
    auto tick_path = writeTicks(TEST_DIR, {
        {99.0, 1.0, TickSide::BID, 1000},    // sets best_bid = 99.0
        {101.0, 1.0, TickSide::ASK, 2000},   // sets best_ask = 101.0, strategy submits BUY
        {100.0, 0.5, TickSide::TRADE, 3000}, // another tick
    });
    ASSERT_FALSE(tick_path.empty());

    LogReader reader;
    ASSERT_TRUE(reader.open(tick_path));

    BacktestEngine engine;
    engine.setStrategy(std::move(strat));

    LogFileSet logSet;
    logSet.tick = std::move(reader);
    engine.setData(logSet);

    engine.run();

    // Strategy should have received ticks and a fill
    EXPECT_EQ(strat_ptr->tick_count, 3);
    EXPECT_GT(strat_ptr->fill_count, 0);
}

TEST_F(BacktestEngineTest, BuyOrderFillsAtBestAsk) {
    auto strat_ptr = new TestStrategy();
    auto strat = std::unique_ptr<TestStrategy>(strat_ptr);
    strat_ptr->auto_submit = true;
    strat_ptr->submit_on_tick_n = 2;
    strat_ptr->submit_side = OrderSide::BUY;
    strat_ptr->submit_price = 100.0;
    strat_ptr->submit_qty = 0.1;

    auto tick_path = writeTicks(TEST_DIR, {
        {99.0,  1.0, TickSide::BID, 1000},
        {101.0, 1.0, TickSide::ASK, 2000},  // strategy submits BUY here
        {102.0, 1.0, TickSide::ASK, 3000},
    });
    ASSERT_FALSE(tick_path.empty());

    LogReader reader;
    ASSERT_TRUE(reader.open(tick_path));

    BacktestEngine engine;
    engine.setStrategy(std::move(strat));

    LogFileSet logSet;
    logSet.tick = std::move(reader);
    engine.setData(logSet);

    engine.run();

    ASSERT_GE(strat_ptr->fills.size(), 1u);
    // BUY fills at best ask, which is 101.0
    EXPECT_NEAR(toDouble(strat_ptr->fills[0].fill_price), 101.0, 1e-9);
}

TEST_F(BacktestEngineTest, SellOrderFillsAtBestBid) {
    auto strat_ptr = new TestStrategy();
    auto strat = std::unique_ptr<TestStrategy>(strat_ptr);
    strat_ptr->auto_submit = true;
    strat_ptr->submit_on_tick_n = 2;
    strat_ptr->submit_side = OrderSide::SELL;
    strat_ptr->submit_price = 100.0;
    strat_ptr->submit_qty = 0.1;

    auto tick_path = writeTicks(TEST_DIR, {
        {99.0,  1.0, TickSide::BID, 1000},
        {101.0, 1.0, TickSide::ASK, 2000},  // strategy submits SELL here
        {98.0,  1.0, TickSide::BID, 3000},
    });
    ASSERT_FALSE(tick_path.empty());

    LogReader reader;
    ASSERT_TRUE(reader.open(tick_path));

    BacktestEngine engine;
    engine.setStrategy(std::move(strat));

    LogFileSet logSet;
    logSet.tick = std::move(reader);
    engine.setData(logSet);

    engine.run();

    ASSERT_GE(strat_ptr->fills.size(), 1u);
    // SELL fills at best bid, which is 99.0
    EXPECT_NEAR(toDouble(strat_ptr->fills[0].fill_price), 99.0, 1e-9);
}

// ============================================================================
// 5. No fill without liquidity
// ============================================================================

TEST_F(BacktestEngineTest, SellOrderDoesNotFillWithoutBid) {
    auto strat_ptr = new TestStrategy();
    auto strat = std::unique_ptr<TestStrategy>(strat_ptr);
    strat_ptr->auto_submit = true;
    strat_ptr->submit_on_tick_n = 1;
    strat_ptr->submit_side = OrderSide::SELL;  // needs best_bid

    // Only ASK ticks — no bids, so SELL won't fill
    auto tick_path = writeTicks(TEST_DIR, {
        {101.0, 1.0, TickSide::ASK, 1000},
        {102.0, 1.0, TickSide::ASK, 2000},
    });
    ASSERT_FALSE(tick_path.empty());

    LogReader reader;
    ASSERT_TRUE(reader.open(tick_path));

    BacktestEngine engine;
    engine.setStrategy(std::move(strat));

    LogFileSet logSet;
    logSet.tick = std::move(reader);
    engine.setData(logSet);

    engine.run();

    // No fill because no bid liquidity
    EXPECT_EQ(strat_ptr->fill_count, 0);
}

TEST_F(BacktestEngineTest, BuyOrderDoesNotFillWithoutAsk) {
    auto strat_ptr = new TestStrategy();
    auto strat = std::unique_ptr<TestStrategy>(strat_ptr);
    strat_ptr->auto_submit = true;
    strat_ptr->submit_on_tick_n = 1;
    strat_ptr->submit_side = OrderSide::BUY;  // needs best_ask

    // Only BID ticks — no asks, so BUY won't fill
    auto tick_path = writeTicks(TEST_DIR, {
        {99.0, 1.0, TickSide::BID, 1000},
        {98.0, 1.0, TickSide::BID, 2000},
    });
    ASSERT_FALSE(tick_path.empty());

    LogReader reader;
    ASSERT_TRUE(reader.open(tick_path));

    BacktestEngine engine;
    engine.setStrategy(std::move(strat));

    LogFileSet logSet;
    logSet.tick = std::move(reader);
    engine.setData(logSet);

    engine.run();

    // No fill because no ask liquidity
    EXPECT_EQ(strat_ptr->fill_count, 0);
}

// ============================================================================
// 6. Equity tracking
// ============================================================================

TEST_F(BacktestEngineTest, EquityCurveRecorded) {
    auto strat = std::make_unique<TestStrategy>();

    auto tick_path = writeTicks(TEST_DIR, {
        {100.0, 1.0, TickSide::BID, 1000},
        {101.0, 1.0, TickSide::ASK, 2000},
        {100.5, 1.0, TickSide::TRADE, 3000},
    });
    ASSERT_FALSE(tick_path.empty());

    LogReader reader;
    ASSERT_TRUE(reader.open(tick_path));

    BacktestEngine engine;
    engine.config().initial_capital = toDecimal(10000.0);
    engine.setStrategy(std::move(strat));

    LogFileSet logSet;
    logSet.tick = std::move(reader);
    engine.setData(logSet);

    engine.run();
    // run() already calls calculateMetrics() at the end

    // At minimum, the initial equity point was recorded
    EXPECT_GT(engine.metrics().equityCurve().size(), 0u);
}

// ============================================================================
// 7. Metrics after run
// ============================================================================

TEST_F(BacktestEngineTest, MetricsCalculatedAfterRun) {
    auto strat_ptr = new TestStrategy();
    auto strat = std::unique_ptr<TestStrategy>(strat_ptr);
    strat_ptr->auto_submit = true;
    strat_ptr->submit_on_tick_n = 2;
    strat_ptr->submit_side = OrderSide::BUY;
    strat_ptr->submit_price = 100.0;
    strat_ptr->submit_qty = 0.1;

    auto tick_path = writeTicks(TEST_DIR, {
        {99.0,  1.0, TickSide::BID, 1000},
        {101.0, 1.0, TickSide::ASK, 2000},  // fills at 101.0
        {102.0, 1.0, TickSide::ASK, 3000},
    });
    ASSERT_FALSE(tick_path.empty());

    LogReader reader;
    ASSERT_TRUE(reader.open(tick_path));

    BacktestEngine engine;
    engine.config().initial_capital = toDecimal(10000.0);
    engine.setStrategy(std::move(strat));

    LogFileSet logSet;
    logSet.tick = std::move(reader);
    engine.setData(logSet);

    engine.run();

    // After run(), metrics are already calculated
    EXPECT_GT(engine.metrics().totalTrades(), 0u);
    EXPECT_GT(engine.metrics().equityCurve().size(), 0u);

    // JSON export works
    auto j = engine.metrics().exportToJSON();
    EXPECT_TRUE(j.contains("total_return"));
    EXPECT_TRUE(j.contains("sharpe_ratio"));
}

// ============================================================================
// 8. Fill modes
// ============================================================================

TEST_F(BacktestEngineTest, ImmediateFillMode) {
    auto strat_ptr = new TestStrategy();
    auto strat = std::unique_ptr<TestStrategy>(strat_ptr);
    strat_ptr->auto_submit = true;
    strat_ptr->submit_on_tick_n = 2;
    strat_ptr->submit_side = OrderSide::BUY;

    auto tick_path = writeTicks(TEST_DIR, {
        {99.0,  1.0, TickSide::BID, 1000},
        {101.0, 1.0, TickSide::ASK, 2000},
    });
    ASSERT_FALSE(tick_path.empty());

    LogReader reader;
    ASSERT_TRUE(reader.open(tick_path));

    BacktestEngine engine;
    engine.config().fill_mode = BacktestConfig::IMMEDIATE;
    engine.setStrategy(std::move(strat));

    LogFileSet logSet;
    logSet.tick = std::move(reader);
    engine.setData(logSet);

    engine.run();

    ASSERT_GE(strat_ptr->fills.size(), 1u);
    EXPECT_NEAR(toDouble(strat_ptr->fills[0].fill_price), 101.0, 1e-9);
}

TEST_F(BacktestEngineTest, ConservativeFillMode) {
    auto strat_ptr = new TestStrategy();
    auto strat = std::unique_ptr<TestStrategy>(strat_ptr);
    strat_ptr->auto_submit = true;
    strat_ptr->submit_on_tick_n = 2;
    strat_ptr->submit_side = OrderSide::BUY;

    auto tick_path = writeTicks(TEST_DIR, {
        {99.0,  1.0, TickSide::BID, 1000},
        {101.0, 1.0, TickSide::ASK, 2000},
    });
    ASSERT_FALSE(tick_path.empty());

    LogReader reader;
    ASSERT_TRUE(reader.open(tick_path));

    BacktestEngine engine;
    engine.config().fill_mode = BacktestConfig::CONSERVATIVE;
    engine.setStrategy(std::move(strat));

    LogFileSet logSet;
    logSet.tick = std::move(reader);
    engine.setData(logSet);

    engine.run();

    ASSERT_GE(strat_ptr->fills.size(), 1u);
    EXPECT_NEAR(toDouble(strat_ptr->fills[0].fill_price), 101.0, 1e-9);
}

// ============================================================================
// 9. Empty data
// ============================================================================

TEST_F(BacktestEngineTest, RunWithNoData) {
    auto strat_ptr = new TestStrategy();
    auto strat = std::unique_ptr<TestStrategy>(strat_ptr);

    auto tick_path = writeTicks(TEST_DIR, {});  // no ticks
    // writeTicks returns empty string if no files written
    // Need at least 1 tick for LogWriter to create a file
    // We'll skip the data setup and just check the strategy was set

    BacktestEngine engine;
    engine.setStrategy(std::move(strat));
    // Don't set data — replayer has no streams

    engine.run();

    EXPECT_EQ(strat_ptr->onload_called, 1);
    EXPECT_EQ(strat_ptr->tick_count, 0);
}
