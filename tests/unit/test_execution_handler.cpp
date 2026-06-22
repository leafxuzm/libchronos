#include <gtest/gtest.h>
#include <atomic>
#include <thread>

#include "chronos/execution/execution_handler.hpp"
#include "chronos/core/types.hpp"

using namespace chronos;
using namespace chronos::execution;
using namespace chronos::trading;

// ============================================================================
// Helpers
// ============================================================================

namespace {

Fill makeFill(uint64_t order_id, uint32_t symbol_id, double price, double qty,
              OrderSide side = OrderSide::BUY, uint32_t strategy_id = 1) {
    Fill f;
    f.order_id = order_id;
    f.symbol_id = symbol_id;
    f.fill_price = toDecimal(price);
    f.fill_quantity = toDecimal(qty);
    f.side = side;
    f.strategy_id = strategy_id;
    f.exchange_id = 1;
    f.receive_timestamp_us = 1000000;
    return f;
}

OrderAck makeAck(uint64_t order_id) {
    OrderAck a;
    a.order_id = order_id;
    a.exchange_order_id = order_id + 10000;
    a.status = OrderStatus::ACCEPTED;
    a.timestamp_us = 2000000;
    return a;
}

OrderReject makeReject(uint64_t order_id, const std::string& reason = "test") {
    OrderReject r;
    r.order_id = order_id;
    r.reason = reason;
    r.timestamp_us = 2000000;
    return r;
}

}  // namespace

// ============================================================================
// 1. Construction
// ============================================================================

TEST(ExecutionHandlerTest, Construction) {
    PositionManager pm;
    ExecutionHandler handler(&pm);

    EXPECT_EQ(handler.positionManager(), &pm);

    auto stats = handler.getStats();
    EXPECT_EQ(stats.fills_processed, 0u);
    EXPECT_EQ(stats.acks_processed, 0u);
    EXPECT_EQ(stats.rejects_processed, 0u);
}

TEST(ExecutionHandlerTest, ConstructionWithNullPM) {
    ExecutionHandler handler(nullptr);
    EXPECT_EQ(handler.positionManager(), nullptr);
}

TEST(ExecutionHandlerTest, ConstructionWithCallbacks) {
    PositionManager pm;
    int called = 0;
    auto cb = [&](const Fill&) { called++; };

    ExecutionHandler handler(&pm, cb);
    handler.onFill(makeFill(1, 1, 100.0, 1.0));
    EXPECT_EQ(called, 1);
}

// ============================================================================
// 2. onFill updates PositionManager
// ============================================================================

TEST(ExecutionHandlerTest, OnFillCreatesPosition) {
    PositionManager pm;
    ExecutionHandler handler(&pm);

    handler.onFill(makeFill(1, 42, 50000.0, 0.5, OrderSide::BUY));

    const Position* pos = pm.getPosition(42);
    ASSERT_NE(pos, nullptr);
    EXPECT_EQ(toDouble(pos->quantity), 0.5);
    EXPECT_EQ(toDouble(pos->average_price), 50000.0);
}

TEST(ExecutionHandlerTest, OnFillAccumulatesPosition) {
    PositionManager pm;
    ExecutionHandler handler(&pm);

    handler.onFill(makeFill(1, 1, 50000.0, 0.3, OrderSide::BUY));
    handler.onFill(makeFill(2, 1, 51000.0, 0.2, OrderSide::BUY));

    const Position* pos = pm.getPosition(1);
    ASSERT_NE(pos, nullptr);
    EXPECT_NEAR(toDouble(pos->quantity), 0.5, 1e-9);
    // Weighted average: (0.3*50000 + 0.2*51000) / 0.5 = 50400
    EXPECT_NEAR(toDouble(pos->average_price), 50400.0, 0.01);
}

TEST(ExecutionHandlerTest, OnFillReversesPosition) {
    PositionManager pm;
    ExecutionHandler handler(&pm);

    // Buy 0.5 @ 50000
    handler.onFill(makeFill(1, 1, 50000.0, 0.5, OrderSide::BUY));
    // Sell 0.8 @ 51000 — reverses from +0.5 to -0.3
    handler.onFill(makeFill(2, 1, 51000.0, 0.8, OrderSide::SELL));

    const Position* pos = pm.getPosition(1);
    ASSERT_NE(pos, nullptr);
    EXPECT_NEAR(toDouble(pos->quantity), -0.3, 1e-9);
}

TEST(ExecutionHandlerTest, OnFillWithNullPMIsSafe) {
    ExecutionHandler handler(nullptr);
    EXPECT_NO_THROW(handler.onFill(makeFill(1, 1, 100.0, 1.0)));

    auto stats = handler.getStats();
    EXPECT_EQ(stats.fills_processed, 1u);
}

// ============================================================================
// 3. onFill invokes notifier
// ============================================================================

TEST(ExecutionHandlerTest, OnFillNotifierReceivesCorrectData) {
    PositionManager pm;
    Fill captured;
    bool called = false;
    auto cb = [&](const Fill& f) { captured = f; called = true; };

    ExecutionHandler handler(&pm, cb);
    Fill sent = makeFill(42, 7, 999.0, 2.5, OrderSide::SELL, 3);
    handler.onFill(sent);

    EXPECT_TRUE(called);
    EXPECT_EQ(captured.order_id, 42u);
    EXPECT_EQ(captured.symbol_id, 7u);
    EXPECT_EQ(toDouble(captured.fill_price), 999.0);
    EXPECT_EQ(toDouble(captured.fill_quantity), 2.5);
    EXPECT_EQ(captured.side, OrderSide::SELL);
    EXPECT_EQ(captured.strategy_id, 3u);
}

TEST(ExecutionHandlerTest, OnFillWithNullNotifierIsSafe) {
    PositionManager pm;
    ExecutionHandler handler(&pm, nullptr);
    EXPECT_NO_THROW(handler.onFill(makeFill(1, 1, 100.0, 1.0)));
}

// ============================================================================
// 4. onOrderAck
// ============================================================================

TEST(ExecutionHandlerTest, OnOrderAckInvokesNotifier) {
    PositionManager pm;
    OrderAck captured;
    bool called = false;
    auto cb = [&](const OrderAck& a) { captured = a; called = true; };

    ExecutionHandler handler(&pm, nullptr, cb);
    OrderAck sent = makeAck(12345);
    handler.onOrderAck(sent);

    EXPECT_TRUE(called);
    EXPECT_EQ(captured.order_id, 12345u);
    EXPECT_EQ(captured.exchange_order_id, 22345u);
}

TEST(ExecutionHandlerTest, OnOrderAckWithNullNotifierIsSafe) {
    PositionManager pm;
    ExecutionHandler handler(&pm, nullptr, nullptr);
    EXPECT_NO_THROW(handler.onOrderAck(makeAck(1)));
}

// ============================================================================
// 5. onOrderReject
// ============================================================================

TEST(ExecutionHandlerTest, OnOrderRejectInvokesNotifier) {
    PositionManager pm;
    OrderReject captured;
    bool called = false;
    auto cb = [&](const OrderReject& r) { captured = r; called = true; };

    ExecutionHandler handler(&pm, nullptr, nullptr, cb);
    OrderReject sent = makeReject(999, "insufficient margin");
    handler.onOrderReject(sent);

    EXPECT_TRUE(called);
    EXPECT_EQ(captured.order_id, 999u);
    EXPECT_EQ(captured.reason, "insufficient margin");
}

TEST(ExecutionHandlerTest, OnOrderRejectWithNullNotifierIsSafe) {
    PositionManager pm;
    ExecutionHandler handler(&pm);
    EXPECT_NO_THROW(handler.onOrderReject(makeReject(1)));
}

// ============================================================================
// 6. Statistics
// ============================================================================

TEST(ExecutionHandlerTest, StatsAccumulate) {
    PositionManager pm;
    ExecutionHandler handler(&pm);

    handler.onFill(makeFill(1, 1, 100.0, 1.0));
    handler.onFill(makeFill(2, 2, 200.0, 2.0));
    handler.onFill(makeFill(3, 3, 300.0, 3.0));
    handler.onOrderAck(makeAck(1));
    handler.onOrderReject(makeReject(4));

    auto stats = handler.getStats();
    EXPECT_EQ(stats.fills_processed, 3u);
    EXPECT_EQ(stats.acks_processed, 1u);
    EXPECT_EQ(stats.rejects_processed, 1u);
    EXPECT_GT(stats.avg_latency_ns, 0u);  // latency tracked
}

TEST(ExecutionHandlerTest, ResetStats) {
    PositionManager pm;
    ExecutionHandler handler(&pm);

    handler.onFill(makeFill(1, 1, 100.0, 1.0));
    handler.onFill(makeFill(2, 2, 200.0, 2.0));

    handler.resetStats();

    auto stats = handler.getStats();
    EXPECT_EQ(stats.fills_processed, 0u);
    EXPECT_EQ(stats.avg_latency_ns, 0u);
}

// ============================================================================
// 7. Thread Safety
// ============================================================================

TEST(ExecutionHandlerTest, ConcurrentFills) {
    PositionManager pm;
    ExecutionHandler handler(&pm);

    std::atomic<int> notify_count{0};
    auto cb = [&](const Fill&) { notify_count++; };

    ExecutionHandler handler2(&pm, cb);

    constexpr int N = 200;
    std::thread t1([&] {
        for (int i = 0; i < N; ++i) {
            handler2.onFill(makeFill(static_cast<uint64_t>(i), 1, 100.0, 0.01));
        }
    });
    std::thread t2([&] {
        for (int i = N; i < N * 2; ++i) {
            handler2.onFill(makeFill(static_cast<uint64_t>(i), 2, 200.0, 0.01));
        }
    });

    t1.join();
    t2.join();

    auto stats = handler2.getStats();
    EXPECT_EQ(stats.fills_processed, static_cast<uint64_t>(N * 2));
    EXPECT_EQ(notify_count.load(), N * 2);

    // PositionManager should have combined all fills
    const Position* pos1 = pm.getPosition(1);
    ASSERT_NE(pos1, nullptr);
    EXPECT_NEAR(toDouble(pos1->quantity), N * 0.01, 0.001);
}
