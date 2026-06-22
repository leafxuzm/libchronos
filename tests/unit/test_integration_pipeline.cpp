/**
 * @file test_integration_pipeline.cpp
 * @brief End-to-end integration: LogReader → TimeReplayer → OrderBookV2 →
 *        StrategyEngine → GridStrategy → simulated fills → LogWriter → verify
 *
 * Mirrors the exact pipeline in apps/trading_engine/main.cpp.
 */

#include "chronos/backtest/time_replayer.hpp"
#include "chronos/core/types.hpp"
#include "chronos/logging/log_reader.hpp"
#include "chronos/logging/log_writer.hpp"
#include "chronos/market_data/orderbook_v2.hpp"
#include "chronos/strategies/grid_strategy.hpp"
#include "chronos/trading/strategy_engine.hpp"

#include <gtest/gtest.h>
#include <filesystem>
#include <cstdio>

using namespace chronos;
using namespace chronos::trading;
using namespace chronos::logging;
using namespace chronos::strategies;
using namespace chronos::market_data;
using namespace chronos::backtest;

namespace fs = std::filesystem;

namespace {

const std::string TEST_DIR = "/tmp/chronos_integration_test";

void cleanDir() {
    std::error_code ec;
    if (fs::exists(TEST_DIR, ec)) {
        fs::remove_all(TEST_DIR, ec);
    }
    fs::create_directories(TEST_DIR);
}

// --- Helper: generate test tick data and write to binary log ---
std::string generateTickLog(const std::string& dir,
                             const std::vector<Tick>& ticks) {
    LogWriter writer;
    LogConfig cfg;
    cfg.log_dir = dir;
    cfg.flush_interval_ms = 10;
    writer.initialize(dir, cfg);

    for (auto& t : ticks) {
        writer.writeTick(t);
    }
    writer.flush();
    writer.stop();
    return dir;
}

// ============================================================================
// 1. Log Round-Trip Integrity (Properties 20-23)
// ============================================================================

TEST(IntegrationPipelineTest, LogRoundTripTicks) {
    cleanDir();

    // Generate test ticks
    std::vector<Tick> original;
    for (int i = 0; i < 10; ++i) {
        Tick t;
        t.exchange_timestamp_us = 1000000 + i * 1000;
        t.receive_timestamp_us  = 1000000 + i * 1000 + 50;
        t.symbol_id = 1;
        t.price     = toDecimal(100.0 + i * 0.5);
        t.quantity  = toDecimal(0.1);
        t.side      = (i % 2 == 0) ? TickSide::BID : TickSide::ASK;
        original.push_back(t);
    }

    generateTickLog(TEST_DIR, original);

    // Read back
    LogFileSet logSet = openLogDirectory(TEST_DIR);
    ASSERT_TRUE(logSet.tick.isOpen());
    ASSERT_EQ(logSet.tick.recordCount(), original.size());

    // Verify each record — binary-exact match
    for (size_t i = 0; i < original.size(); ++i) {
        auto* readback = logSet.tick.tickAt(i);
        ASSERT_NE(readback, nullptr);
        EXPECT_EQ(readback->exchange_timestamp_us, original[i].exchange_timestamp_us);
        EXPECT_EQ(readback->receive_timestamp_us,  original[i].receive_timestamp_us);
        EXPECT_EQ(readback->symbol_id,              original[i].symbol_id);
        EXPECT_EQ(readback->price.raw_value(),      original[i].price.raw_value());
        EXPECT_EQ(readback->quantity.raw_value(),   original[i].quantity.raw_value());
        EXPECT_EQ(readback->side,                   original[i].side);
    }
}

TEST(IntegrationPipelineTest, LogRoundTripOrdersAndFills) {
    cleanDir();

    // Write orders + fills
    LogWriter writer;
    LogConfig cfg;
    cfg.log_dir = TEST_DIR;
    cfg.flush_interval_ms = 10;
    writer.initialize(TEST_DIR, cfg);

    OrderRequest o1{};
    o1.order_id = 1001;
    o1.timestamp_us = 2000000;
    o1.symbol_id = 1;
    o1.price = toDecimal(100.0);
    o1.quantity = toDecimal(1.0);
    o1.side = OrderSide::BUY;
    o1.type = OrderType::LIMIT;
    writer.writeOrder(o1);

    Fill f1{};
    f1.execution_id = 5001;
    f1.fill_id = 6001;
    f1.order_id = 1001;
    f1.exchange_timestamp_us = 3000000;
    f1.fill_price = toDecimal(100.0);
    f1.fill_quantity = toDecimal(1.0);
    f1.symbol_id = 1;
    f1.side = OrderSide::BUY;
    f1.is_maker = 1;
    writer.writeFill(f1);

    writer.flush();
    writer.stop();

    // Read back
    LogFileSet logSet = openLogDirectory(TEST_DIR);
    ASSERT_TRUE(logSet.order.isOpen());
    ASSERT_TRUE(logSet.fill.isOpen());
    ASSERT_EQ(logSet.order.recordCount(), 1u);
    ASSERT_EQ(logSet.fill.recordCount(), 1u);

    auto* ro = logSet.order.orderAt(0);
    ASSERT_NE(ro, nullptr);
    EXPECT_EQ(ro->order_id, o1.order_id);
    EXPECT_EQ(ro->price.raw_value(), o1.price.raw_value());
    EXPECT_EQ(ro->quantity.raw_value(), o1.quantity.raw_value());
    EXPECT_EQ(ro->side, o1.side);
    EXPECT_EQ(ro->type, o1.type);

    auto* rf = logSet.fill.fillAt(0);
    ASSERT_NE(rf, nullptr);
    EXPECT_EQ(rf->execution_id, f1.execution_id);
    EXPECT_EQ(rf->order_id, f1.order_id);
    EXPECT_EQ(rf->fill_price.raw_value(), f1.fill_price.raw_value());
    EXPECT_EQ(rf->fill_quantity.raw_value(), f1.fill_quantity.raw_value());
    EXPECT_EQ(rf->is_maker, f1.is_maker);
}

// ============================================================================
// 2. Full Pipeline: LogReader → OrderBook → StrategyEngine → GridStrategy → Fill
// ============================================================================

class FullPipelineTest : public ::testing::Test {
protected:
    void SetUp() override { cleanDir(); }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(TEST_DIR, ec);
    }
};

TEST_F(FullPipelineTest, TickToOrderViaGridStrategy) {
    // --- Generate tick data: price oscillates 100 → 101 → 99 → 100 ---
    // Emit BID+ASK pairs so both sides of the book are populated (getMidPrice requires both)
    std::vector<Tick> ticks;
    double prices[] = {100.0, 100.5, 101.0, 100.5, 100.0, 99.5, 99.0, 99.5, 100.0};
    for (size_t i = 0; i < sizeof(prices)/sizeof(prices[0]); ++i) {
        Tick t_ask;
        t_ask.exchange_timestamp_us = 1000000 + i * 100000;
        t_ask.receive_timestamp_us  = t_ask.exchange_timestamp_us + 50;
        t_ask.symbol_id = 1;
        t_ask.price = toDecimal(prices[i] + 0.05);
        t_ask.quantity = toDecimal(1.0);
        t_ask.side = TickSide::ASK;
        ticks.push_back(t_ask);

        Tick t_bid;
        t_bid.exchange_timestamp_us = t_ask.exchange_timestamp_us + 100;
        t_bid.receive_timestamp_us  = t_bid.exchange_timestamp_us + 50;
        t_bid.symbol_id = 1;
        t_bid.price = toDecimal(prices[i] - 0.05);
        t_bid.quantity = toDecimal(1.0);
        t_bid.side = TickSide::BID;
        ticks.push_back(t_bid);
    }

    generateTickLog(TEST_DIR, ticks);

    // --- Open logs for replay ---
    LogFileSet logSet = openLogDirectory(TEST_DIR);
    ASSERT_TRUE(logSet.tick.isOpen());

    // --- Set up order book ---
    OrderBookV2 book;

    // --- Set up strategy engine ---
    StrategyEngine engine;
    engine.setAvailableCapital(toDecimal(100000.0));

    // --- Register GridStrategy (grid: 95–105, 5 levels) ---
    GridStrategy::Config gridCfg;
    gridCfg.symbol_id   = 1;
    gridCfg.grid_low    = 95.0;
    gridCfg.grid_high   = 105.0;
    gridCfg.grid_levels = 5;
    gridCfg.quantity    = 0.1;

    auto strategy = std::make_unique<GridStrategy>(gridCfg);
    engine.registerStrategy(std::move(strategy));
    engine.start();

    // --- Set up time replayer ---
    TimeReplayer replayer;
    replayer.addStream(logSet.tick);

    uint64_t tickCount = 0;
    uint64_t orderCount = 0;
    uint64_t fillCount = 0;

    replayer.setTickCallback([&](const Tick& tick) {
        tickCount++;
        book.update(tick);
        engine.pushTick(tick);
    });

    replayer.setOrderCallback([](const OrderRequest&) {});
    replayer.setFillCallback([](const Fill&) {});

    // --- Run replay ---
    while (!replayer.isExhausted()) {
        replayer.advanceToNextEvent();
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    // Give engine time to process all ticks
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Drain all orders that were generated during the replay
    OrderRequest order;
    while (engine.popOrder(order)) {
        // Skip cancel orders (quantity == 0 signals cancellation)
        if (order.quantity == Decimal(0)) continue;

        orderCount++;

        Fill fill;
        fill.order_id = order.order_id;
        fill.symbol_id = order.symbol_id;
        fill.fill_quantity = order.quantity;
        fill.side = order.side;
        fill.exchange_timestamp_us = 0;
        fill.receive_timestamp_us = 0;
        fill.strategy_id = order.strategy_id;

        if (order.side == OrderSide::BUY) {
            auto bestAsk = book.getBestAsk();
            if (!bestAsk) continue;
            fill.fill_price = *bestAsk;
        } else {
            auto bestBid = book.getBestBid();
            if (!bestBid) continue;
            fill.fill_price = *bestBid;
        }

        engine.pushFill(fill);
        fillCount++;
    }

    // Give engine time to process fills
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    engine.stop();

    // --- Verify pipeline flow ---
    auto stats = engine.getStats();
    EXPECT_GT(stats.ticks_processed, 0u);
    EXPECT_GT(stats.orders_submitted, 0u) << "GridStrategy should have placed orders";
    EXPECT_GT(stats.fills_processed, 0u) << "Fills should have been processed";

    // Counts should be internally consistent
    EXPECT_GE(stats.orders_submitted, orderCount)
        << "orders_submitted >= drained order count";
    EXPECT_GE(stats.fills_processed, fillCount)
        << "fills_processed >= simulated fill count";
}

TEST_F(FullPipelineTest, PriceOscillationTriggersGridTrades) {
    // Grid: 95-105, 5 levels → spacing=(105-95)/5=2.0 → levels at 95,97,99,101,103,105
    // Initial price 100 → BUY below mid at 99,97,95; SELL above mid at 101,103,105
    // Push price down to 96 → should trigger buy orders at 97,95

    std::vector<Tick> ticks;
    // Start at 100 with bid/ask spread
    for (int i = 0; i < 20; ++i) {
        double base = 100.0;
        if (i >= 10) base = 96.0;  // drop below grid level 97.5

        Tick t_ask;
        t_ask.exchange_timestamp_us = 1000000 + i * 50000;
        t_ask.receive_timestamp_us  = t_ask.exchange_timestamp_us + 50;
        t_ask.symbol_id = 1;
        t_ask.price = toDecimal(base + 0.1);  // ask
        t_ask.quantity = toDecimal(1.0);
        t_ask.side = TickSide::ASK;
        ticks.push_back(t_ask);

        Tick t_bid;
        t_bid.exchange_timestamp_us = t_ask.exchange_timestamp_us + 100;
        t_bid.receive_timestamp_us  = t_bid.exchange_timestamp_us + 50;
        t_bid.symbol_id = 1;
        t_bid.price = toDecimal(base - 0.1);  // bid
        t_bid.quantity = toDecimal(1.0);
        t_bid.side = TickSide::BID;
        ticks.push_back(t_bid);
    }

    generateTickLog(TEST_DIR, ticks);

    LogFileSet logSet = openLogDirectory(TEST_DIR);
    ASSERT_TRUE(logSet.tick.isOpen());

    OrderBookV2 book;
    StrategyEngine engine;
    engine.setAvailableCapital(toDecimal(100000.0));

    GridStrategy::Config gridCfg;
    gridCfg.symbol_id   = 1;
    gridCfg.grid_low    = 95.0;
    gridCfg.grid_high   = 105.0;
    gridCfg.grid_levels = 5;
    gridCfg.quantity    = 0.1;

    engine.registerStrategy(std::make_unique<GridStrategy>(gridCfg));
    engine.start();

    TimeReplayer replayer;
    replayer.addStream(logSet.tick);

    std::vector<OrderRequest> allOrders;
    std::vector<Fill> allFills;

    replayer.setTickCallback([&](const Tick& tick) {
        book.update(tick);
        engine.pushTick(tick);
    });

    replayer.setOrderCallback([](const OrderRequest&) {});
    replayer.setFillCallback([](const Fill&) {});

    while (!replayer.isExhausted()) {
        replayer.advanceToNextEvent();
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    // Wait for engine to process
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Drain orders
    OrderRequest order;
    while (engine.popOrder(order)) {
        // Skip cancel orders (quantity == 0 signals cancellation)
        if (order.quantity == Decimal(0)) continue;

        allOrders.push_back(order);

        Fill fill;
        fill.order_id = order.order_id;
        fill.symbol_id = order.symbol_id;
        fill.fill_quantity = order.quantity;
        fill.side = order.side;
        fill.exchange_timestamp_us = 0;
        fill.receive_timestamp_us = 0;
        fill.strategy_id = order.strategy_id;

        if (order.side == OrderSide::BUY) {
            auto bestAsk = book.getBestAsk();
            if (!bestAsk) continue;
            fill.fill_price = *bestAsk;
        } else {
            auto bestBid = book.getBestBid();
            if (!bestBid) continue;
            fill.fill_price = *bestBid;
        }

        engine.pushFill(fill);
        allFills.push_back(fill);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    engine.stop();

    // Verify: at least some orders were generated
    ASSERT_FALSE(allOrders.empty()) << "Expected grid orders when price moves";
    ASSERT_FALSE(allFills.empty()) << "Expected fills for grid orders";

    // Grid: 95–105 / 5 levels → spacing=2.0 → levels at 95, 97, 99, 101, 103, 105
    // GridStrategy uses aggressive pricing (+0.5 offsets for cross-spread fills),
    // so orders land near but not exactly at grid levels.  Verify prices stay
    // within the valid grid range [low - margin, high + margin].
    for (auto& o : allOrders) {
        double price = toDouble(o.price);
        EXPECT_GE(price, gridCfg.grid_low - 1.0)
            << "Order price " << price << " below grid range";
        EXPECT_LE(price, gridCfg.grid_high + 1.0)
            << "Order price " << price << " above grid range";
    }
}

// ============================================================================
// 3. ZMQ Bridge Round-Trip (in-process subscriber)
// ============================================================================

#include <zmq.h>

TEST(IntegrationPipelineTest, ZmqBridgePublishAndReceive) {
    cleanDir();

    // --- Publisher ---
    void* ctx = zmq_ctx_new();
    ASSERT_NE(ctx, nullptr);

    void* pub = zmq_socket(ctx, ZMQ_PUB);
    ASSERT_NE(pub, nullptr);

    int linger = 100;
    zmq_setsockopt(pub, ZMQ_LINGER, &linger, sizeof(linger));
    int hwm = 1000;
    zmq_setsockopt(pub, ZMQ_SNDHWM, &hwm, sizeof(hwm));

    int rc = zmq_bind(pub, "tcp://127.0.0.1:0");
    ASSERT_EQ(rc, 0) << "zmq_bind failed: " << zmq_strerror(errno);

    // Retrieve ephemeral port assigned by OS
    char endpoint[128];
    size_t ep_size = sizeof(endpoint);
    rc = zmq_getsockopt(pub, ZMQ_LAST_ENDPOINT, endpoint, &ep_size);
    ASSERT_EQ(rc, 0) << "zmq_getsockopt(ZMQ_LAST_ENDPOINT) failed";

    // --- Subscriber ---
    void* sub = zmq_socket(ctx, ZMQ_SUB);
    ASSERT_NE(sub, nullptr);

    int timeout = 2000;
    zmq_setsockopt(sub, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
    zmq_setsockopt(sub, ZMQ_LINGER, &linger, sizeof(linger));

    rc = zmq_connect(sub, endpoint);
    ASSERT_EQ(rc, 0) << "zmq_connect failed: " << zmq_strerror(errno);

    // Subscribe to all topics
    zmq_setsockopt(sub, ZMQ_SUBSCRIBE, "", 0);

    // Allow connection to establish
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // --- Publish a Tick ---
    Tick tick{};
    tick.exchange_timestamp_us = 12345678;
    tick.receive_timestamp_us  = 12345700;
    tick.symbol_id = 1;
    tick.price = toDecimal(50000.0);
    tick.quantity = toDecimal(0.5);
    tick.side = TickSide::BID;

    // Wire format: 4B topic + binary record
    const char topic[] = "TICK";
    uint8_t msg[4 + sizeof(Tick)];
    std::memcpy(msg, topic, 4);
    std::memcpy(msg + 4, &tick, sizeof(Tick));

    rc = zmq_send(pub, msg, sizeof(msg), 0);
    ASSERT_GT(rc, 0) << "zmq_send failed";

    // --- Publish a Fill ---
    Fill fill{};
    fill.execution_id = 999;
    fill.order_id = 555;
    fill.exchange_timestamp_us = 99999999;
    fill.fill_price = toDecimal(50001.0);
    fill.fill_quantity = toDecimal(1.0);
    fill.symbol_id = 1;
    fill.side = OrderSide::BUY;
    fill.is_maker = 1;

    const char topic_f[] = "FILL";
    uint8_t msg_f[4 + sizeof(Fill)];
    std::memcpy(msg_f, topic_f, 4);
    std::memcpy(msg_f + 4, &fill, sizeof(Fill));

    rc = zmq_send(pub, msg_f, sizeof(msg_f), 0);
    ASSERT_GT(rc, 0);

    // --- Receive and verify ---
    uint8_t rbuf[4 + 128];
    int n = zmq_recv(sub, rbuf, sizeof(rbuf), 0);
    ASSERT_GT(n, 0) << "No message received from ZMQ";
    ASSERT_GE(n, 4 + (int)sizeof(Tick));

    char rtopic[5] = {0};
    std::memcpy(rtopic, rbuf, 4);
    EXPECT_STREQ(rtopic, "TICK");

    auto* rtick = reinterpret_cast<const Tick*>(rbuf + 4);
    EXPECT_EQ(rtick->exchange_timestamp_us, tick.exchange_timestamp_us);
    EXPECT_EQ(rtick->price.raw_value(), tick.price.raw_value());
    EXPECT_EQ(rtick->quantity.raw_value(), tick.quantity.raw_value());

    // Second message (Fill)
    n = zmq_recv(sub, rbuf, sizeof(rbuf), 0);
    ASSERT_GT(n, 0) << "Second message not received";
    std::memcpy(rtopic, rbuf, 4);
    EXPECT_STREQ(rtopic, "FILL");

    auto* rfill = reinterpret_cast<const Fill*>(rbuf + 4);
    EXPECT_EQ(rfill->execution_id, fill.execution_id);
    EXPECT_EQ(rfill->fill_price.raw_value(), fill.fill_price.raw_value());
    EXPECT_EQ(rfill->is_maker, fill.is_maker);

    // --- Cleanup ---
    zmq_close(sub);
    zmq_close(pub);
    zmq_ctx_destroy(ctx);
}

}  // namespace
