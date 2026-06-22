#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>

#include "chronos/execution/order_gateway.hpp"
#include "chronos/core/types.hpp"
#include "chronos/core/config.hpp"

using namespace chronos;
using namespace chronos::execution;

// ============================================================================
// Helpers
// ============================================================================

namespace {

ExchangeConfig makeConfig(const std::string& name = "test_exchange") {
    ExchangeConfig cfg;
    cfg.name = name;
    cfg.websocket_url = "ws://localhost:9999";
    return cfg;
}

OrderRequest makeOrder(uint32_t symbol_id, uint32_t exchange_id,
                       double price, double qty, OrderSide side = OrderSide::BUY) {
    OrderRequest o;
    o.symbol_id = symbol_id;
    o.exchange_id = exchange_id;
    o.price = toDecimal(price);
    o.quantity = toDecimal(qty);
    o.side = side;
    o.type = OrderType::LIMIT;
    o.tif = TimeInForce::GTC;
    o.order_id = static_cast<uint64_t>(symbol_id * 1000 + price * 100);
    o.timestamp_us = 1;
    return o;
}

OrderRequest makeCancel(uint64_t order_id, uint32_t exchange_id) {
    OrderRequest o;
    o.order_id = order_id;
    o.exchange_id = exchange_id;
    o.quantity = Decimal(0);
    o.timestamp_us = 1;
    return o;
}

Fill makeFill(uint64_t order_id, double price, double qty,
              uint32_t strategy_id = 1) {
    Fill f;
    f.order_id = order_id;
    f.fill_price = toDecimal(price);
    f.fill_quantity = toDecimal(qty);
    f.strategy_id = strategy_id;
    f.symbol_id = 1;
    f.exchange_id = 0;
    return f;
}

OrderAck makeAck(uint64_t order_id) {
    OrderAck a;
    a.order_id = order_id;
    a.exchange_order_id = order_id + 10000;
    a.status = OrderStatus::ACCEPTED;
    a.timestamp_us = 2;
    return a;
}

OrderReject makeReject(uint64_t order_id, const std::string& reason = "test reject") {
    OrderReject r;
    r.order_id = order_id;
    r.reason = reason;
    r.timestamp_us = 2;
    return r;
}

// Spin-wait helper
void waitFor(std::function<bool()> condition, int timeout_ms = 1000) {
    auto deadline = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(timeout_ms);
    while (!condition() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
}

}  // namespace

// ============================================================================
// 1. Construction & Lifecycle
// ============================================================================

TEST(OrderGatewayTest, Construction) {
    OrderGateway::OrderQueue queue;
    auto cfg = makeConfig();
    // Pass explicit exchange_id = 7 to match what strategies may set
    OrderGateway gw(cfg, &queue, nullptr, 7, false);

    EXPECT_EQ(gw.exchangeId(), 7u);
    EXPECT_EQ(gw.exchangeName(), "test_exchange");
    EXPECT_FALSE(gw.isRunning());
    EXPECT_EQ(gw.pendingCount(), 0u);
}

TEST(OrderGatewayTest, StartStop) {
    OrderGateway::OrderQueue queue;
    OrderGateway gw(makeConfig(), &queue, nullptr);

    gw.start();
    EXPECT_TRUE(gw.isRunning());
    gw.stop();
    EXPECT_FALSE(gw.isRunning());
}

TEST(OrderGatewayTest, DoubleStartIsSafe) {
    OrderGateway::OrderQueue queue;
    OrderGateway gw(makeConfig(), &queue, nullptr);

    gw.start();
    gw.start();  // should be no-op
    EXPECT_TRUE(gw.isRunning());
    gw.stop();
    EXPECT_FALSE(gw.isRunning());
}

TEST(OrderGatewayTest, DoubleStopIsSafe) {
    OrderGateway::OrderQueue queue;
    OrderGateway gw(makeConfig(), &queue, nullptr);

    gw.start();
    gw.stop();
    gw.stop();  // should be no-op
    EXPECT_FALSE(gw.isRunning());
}

TEST(OrderGatewayTest, DestructorStopsThread) {
    OrderGateway::OrderQueue queue;
    {
        OrderGateway gw(makeConfig(), &queue, nullptr);
        gw.start();
        EXPECT_TRUE(gw.isRunning());
    }
    // No crash = pass
    SUCCEED();
}

// ============================================================================
// 2. Exchange ID Filtering
// ============================================================================

TEST(OrderGatewayTest, MatchingExchangeIdProcessed) {
    OrderGateway::OrderQueue queue;
    OrderGateway gw(makeConfig("binance"), &queue, nullptr);
    uint32_t ex_id = gw.exchangeId();

    gw.start();
    queue.try_push(makeOrder(1, ex_id, 100.0, 1.0));

    waitFor([&] { return gw.pendingCount() > 0; });
    EXPECT_EQ(gw.pendingCount(), 1u);
    gw.stop();
}

TEST(OrderGatewayTest, MismatchedExchangeIdDropped) {
    OrderGateway::OrderQueue queue;
    OrderGateway gw(makeConfig("binance"), &queue, nullptr);
    uint32_t wrong_id = gw.exchangeId() + 1;

    gw.start();
    queue.try_push(makeOrder(1, wrong_id, 100.0, 1.0));

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(gw.pendingCount(), 0u);

    auto stats = gw.getStats();
    EXPECT_EQ(stats.exchange_id_mismatch, 1u);
    gw.stop();
}

// ============================================================================
// 3. New Order Flow
// ============================================================================

TEST(OrderGatewayTest, NewOrderTrackedInPending) {
    OrderGateway::OrderQueue queue;
    OrderGateway gw(makeConfig(), &queue, nullptr);
    uint32_t ex_id = gw.exchangeId();

    gw.start();
    queue.try_push(makeOrder(1, ex_id, 100.0, 1.0));

    waitFor([&] { return gw.pendingCount() > 0; });
    EXPECT_EQ(gw.pendingCount(), 1u);

    auto stats = gw.getStats();
    EXPECT_EQ(stats.orders_received, 1u);
    EXPECT_EQ(stats.orders_sent, 1u);
    gw.stop();
}

TEST(OrderGatewayTest, MultipleNewOrders) {
    OrderGateway::OrderQueue queue;
    OrderGateway gw(makeConfig(), &queue, nullptr);
    uint32_t ex_id = gw.exchangeId();

    gw.start();
    for (int i = 0; i < 5; ++i) {
        auto o = makeOrder(static_cast<uint32_t>(i + 1), ex_id,
                           100.0 + i, 1.0 + i * 0.5);
        o.order_id = static_cast<uint64_t>(1000 + i);
        queue.try_push(o);
    }

    waitFor([&] { return gw.pendingCount() >= 5; });
    EXPECT_EQ(gw.pendingCount(), 5u);

    auto stats = gw.getStats();
    EXPECT_EQ(stats.orders_sent, 5u);
    gw.stop();
}

// ============================================================================
// 4. Cancel Flow
// ============================================================================

TEST(OrderGatewayTest, CancelRemovesPending) {
    OrderGateway::OrderQueue queue;
    OrderGateway gw(makeConfig(), &queue, nullptr);
    uint32_t ex_id = gw.exchangeId();

    gw.start();
    uint64_t oid = 5000;
    auto order = makeOrder(1, ex_id, 100.0, 1.0);
    order.order_id = oid;
    queue.try_push(order);

    waitFor([&] { return gw.pendingCount() > 0; });

    queue.try_push(makeCancel(oid, ex_id));
    waitFor([&] { return gw.pendingCount() == 0; });

    EXPECT_EQ(gw.pendingCount(), 0u);
    auto stats = gw.getStats();
    EXPECT_EQ(stats.orders_cancelled, 1u);
    gw.stop();
}

TEST(OrderGatewayTest, CancelUnknownOrder) {
    OrderGateway::OrderQueue queue;
    OrderGateway gw(makeConfig(), &queue, nullptr);
    uint32_t ex_id = gw.exchangeId();

    gw.start();
    queue.try_push(makeCancel(99999, ex_id));

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    // Should not crash
    auto stats = gw.getStats();
    EXPECT_EQ(stats.orders_cancelled, 1u);
    gw.stop();
}

// ============================================================================
// 5. Modify Flow
// ============================================================================

TEST(OrderGatewayTest, ModifyUpdatesPending) {
    OrderGateway::OrderQueue queue;
    OrderGateway gw(makeConfig(), &queue, nullptr);
    uint32_t ex_id = gw.exchangeId();

    gw.start();
    uint64_t oid = 7000;
    auto order = makeOrder(1, ex_id, 100.0, 1.0);
    order.order_id = oid;
    queue.try_push(order);

    waitFor([&] { return gw.pendingCount() > 0; });

    // Send modify with same order_id
    auto modified = makeOrder(1, ex_id, 105.0, 2.0);
    modified.order_id = oid;
    queue.try_push(modified);

    waitFor([&] {
        auto stats = gw.getStats();
        return stats.orders_modified > 0;
    });

    auto stats = gw.getStats();
    EXPECT_EQ(stats.orders_modified, 1u);
    EXPECT_EQ(stats.orders_sent, 1u);   // only the original new order
    EXPECT_EQ(gw.pendingCount(), 1u);   // still pending (modified, not removed)
    gw.stop();
}

// ============================================================================
// 6. Fill Injection
// ============================================================================

TEST(OrderGatewayTest, FillInvokesCallback) {
    OrderGateway::OrderQueue queue;
    std::atomic<int> fill_count{0};
    Fill last_fill;

    FillCallback cb = [&](const Fill& f) {
        fill_count++;
        last_fill = f;
    };

    // simulate_fills=false so we control exactly when fills fire
    OrderGateway gw(makeConfig(), &queue, cb, 0, false);
    uint32_t ex_id = gw.exchangeId();

    gw.start();

    // Push an order so trackPending creates the pending order entry
    auto order = makeOrder(1, ex_id, 100.0, 1.0);
    order.order_id = 100;
    queue.try_push(order);

    waitFor([&] { return gw.pendingCount() > 0; });

    Fill f = makeFill(100, 99.5, 0.5);
    gw.injectFill(f);

    gw.stop();

    EXPECT_EQ(fill_count.load(), 1);
    EXPECT_EQ(last_fill.order_id, 100u);
    EXPECT_EQ(toDouble(last_fill.fill_price), 99.5);
}

TEST(OrderGatewayTest, PartialFillUpdatesQuantity) {
    OrderGateway::OrderQueue queue;
    uint32_t ex_id = 0;  // will be set by gateway
    OrderGateway gw(makeConfig("binance"), &queue, nullptr);
    ex_id = gw.exchangeId();

    gw.start();
    uint64_t oid = 8000;
    auto order = makeOrder(1, ex_id, 100.0, 1.0);
    order.order_id = oid;
    queue.try_push(order);

    waitFor([&] { return gw.pendingCount() > 0; });

    Fill f = makeFill(oid, 100.0, 0.3);
    gw.injectFill(f);

    auto stats = gw.getStats();
    EXPECT_EQ(stats.fills_received, 1u);
    EXPECT_EQ(gw.pendingCount(), 1u);  // still pending (partial fill)
    gw.stop();
}

TEST(OrderGatewayTest, FullFillRemovesPending) {
    OrderGateway::OrderQueue queue;
    OrderGateway gw(makeConfig("binance"), &queue, nullptr);
    uint32_t ex_id = gw.exchangeId();

    gw.start();
    uint64_t oid = 9000;
    auto order = makeOrder(1, ex_id, 100.0, 1.0);
    order.order_id = oid;
    queue.try_push(order);

    waitFor([&] { return gw.pendingCount() > 0; });

    Fill f = makeFill(oid, 100.0, 1.0);
    gw.injectFill(f);

    EXPECT_EQ(gw.pendingCount(), 0u);  // fully filled, removed
    auto stats = gw.getStats();
    EXPECT_EQ(stats.fills_received, 1u);
    gw.stop();
}

TEST(OrderGatewayTest, FillForUnknownOrder) {
    OrderGateway::OrderQueue queue;
    OrderGateway gw(makeConfig(), &queue, nullptr);

    Fill f = makeFill(99999, 100.0, 1.0);
    gw.injectFill(f);

    auto stats = gw.getStats();
    EXPECT_EQ(stats.fill_errors, 1u);
    EXPECT_EQ(stats.fills_received, 0u);
}

// ============================================================================
// 7. Ack / Reject Injection
// ============================================================================

TEST(OrderGatewayTest, OrderAckSetsAccepted) {
    OrderGateway::OrderQueue queue;
    OrderGateway gw(makeConfig("binance"), &queue, nullptr);
    uint32_t ex_id = gw.exchangeId();

    gw.start();
    uint64_t oid = 10000;
    auto order = makeOrder(1, ex_id, 100.0, 1.0);
    order.order_id = oid;
    queue.try_push(order);

    waitFor([&] { return gw.pendingCount() > 0; });

    gw.injectOrderAck(makeAck(oid));
    // Ack doesn't affect pending count; order is still pending
    EXPECT_EQ(gw.pendingCount(), 1u);
    gw.stop();
}

TEST(OrderGatewayTest, OrderRejectFinalizes) {
    OrderGateway::OrderQueue queue;
    OrderGateway gw(makeConfig("binance"), &queue, nullptr);
    uint32_t ex_id = gw.exchangeId();

    gw.start();
    uint64_t oid = 11000;
    auto order = makeOrder(1, ex_id, 100.0, 1.0);
    order.order_id = oid;
    queue.try_push(order);

    waitFor([&] { return gw.pendingCount() > 0; });

    gw.injectOrderReject(makeReject(oid));
    EXPECT_EQ(gw.pendingCount(), 0u);

    auto stats = gw.getStats();
    EXPECT_EQ(stats.orders_rejected, 1u);
    gw.stop();
}

// ============================================================================
// 8. Statistics
// ============================================================================

TEST(OrderGatewayTest, StatsAccumulate) {
    OrderGateway::OrderQueue queue;
    OrderGateway gw(makeConfig("binance"), &queue, nullptr);
    uint32_t ex_id = gw.exchangeId();
    uint32_t wrong_id = ex_id + 1;

    gw.start();

    // 2 new orders (matching), 1 mismatch, 1 cancel
    uint64_t oid1 = 12001, oid2 = 12002;
    auto o1 = makeOrder(1, ex_id, 100.0, 1.0); o1.order_id = oid1;
    auto o2 = makeOrder(2, ex_id, 200.0, 2.0); o2.order_id = oid2;
    auto bad = makeOrder(3, wrong_id, 300.0, 3.0);
    queue.try_push(o1);
    queue.try_push(o2);
    queue.try_push(bad);

    waitFor([&] { return gw.pendingCount() >= 2; });

    queue.try_push(makeCancel(oid1, ex_id));
    waitFor([&] { return gw.pendingCount() <= 1; });

    auto stats = gw.getStats();
    EXPECT_EQ(stats.orders_received, 4u);
    EXPECT_EQ(stats.orders_sent, 2u);
    EXPECT_EQ(stats.exchange_id_mismatch, 1u);
    EXPECT_EQ(stats.orders_cancelled, 1u);
    EXPECT_EQ(stats.pending_order_count, 1u);
    gw.stop();
}

TEST(OrderGatewayTest, ResetStats) {
    OrderGateway::OrderQueue queue;
    OrderGateway gw(makeConfig("binance"), &queue, nullptr);
    uint32_t ex_id = gw.exchangeId();

    gw.start();
    auto o = makeOrder(1, ex_id, 100.0, 1.0);
    o.order_id = 13000;
    queue.try_push(o);

    waitFor([&] { return gw.pendingCount() > 0; });

    gw.resetStats();

    auto stats = gw.getStats();
    EXPECT_EQ(stats.orders_received, 0u);
    EXPECT_EQ(stats.orders_sent, 0u);
    EXPECT_EQ(stats.avg_send_latency_us, 0u);
    // pending order count is still tracked (it's not a stat reset)
    EXPECT_EQ(stats.pending_order_count, 1u);
    gw.stop();
}

// ============================================================================
// 9. Thread Safety
// ============================================================================

TEST(OrderGatewayTest, ConcurrentOrders) {
    OrderGateway::OrderQueue queue;
    OrderGateway gw(makeConfig("binance"), &queue, nullptr);
    uint32_t ex_id = gw.exchangeId();

    gw.start();

    constexpr int N = 20;
    std::thread producers[4];
    for (int t = 0; t < 4; ++t) {
        producers[t] = std::thread([&, t] {
            for (int i = 0; i < N / 4; ++i) {
                auto o = makeOrder(1, ex_id, 100.0 + t * 100 + i, 1.0);
                o.order_id = static_cast<uint64_t>(t * 10000 + i);
                queue.try_push(o);
            }
        });
    }

    for (auto& t : producers) t.join();

    waitFor([&] { return gw.pendingCount() >= static_cast<size_t>(N); }, 2000);
    EXPECT_EQ(gw.pendingCount(), static_cast<size_t>(N));

    gw.stop();
}

TEST(OrderGatewayTest, NullCallbackIsSafe) {
    OrderGateway::OrderQueue queue;
    OrderGateway gw(makeConfig(), &queue, nullptr);

    // Inject fill with null callback — should not crash
    Fill f = makeFill(1, 100.0, 1.0);
    EXPECT_NO_THROW(gw.injectFill(f));
}
