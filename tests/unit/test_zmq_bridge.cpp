#include <gtest/gtest.h>
#include <chrono>
#include <cstring>
#include <thread>

#include "chronos/logging/zmq_bridge.hpp"
#include "chronos/core/types.hpp"

#include <zmq.h>

using namespace chronos;
using namespace chronos::logging;

// ============================================================================
// Helpers
// ============================================================================

namespace {

Tick makeTick(uint64_t ts, double price) {
    Tick t;
    t.exchange_timestamp_us = ts;
    t.price = toDecimal(price);
    t.quantity = toDecimal(1.0);
    t.symbol_id = 1;
    t.side = TickSide::BID;
    return t;
}

OrderRequest makeOrder(uint64_t id) {
    OrderRequest o;
    o.order_id = id;
    o.price = toDecimal(100.0);
    o.quantity = toDecimal(0.5);
    o.symbol_id = 1;
    o.side = OrderSide::BUY;
    o.type = OrderType::LIMIT;
    o.tif = TimeInForce::GTC;
    return o;
}

Fill makeFill(uint64_t oid) {
    Fill f;
    f.execution_id = oid * 10;
    f.order_id = oid;
    f.fill_price = toDecimal(50.0);
    f.fill_quantity = toDecimal(0.1);
    f.symbol_id = 1;
    f.side = OrderSide::BUY;
    return f;
}

// Unique port counter to avoid conflicts
int nextPort() {
    static int port = 15600;
    return port++;
}

}  // namespace

// ============================================================================
// 1. Lifecycle
// ============================================================================

TEST(ZMQBridgeTest, InitializeAndStop) {
    ZMQBridge bridge;
    ZMQConfig cfg;
    cfg.bind_address = "tcp://127.0.0.1:" + std::to_string(nextPort());
    EXPECT_TRUE(bridge.initialize(cfg));
    EXPECT_TRUE(bridge.isRunning());
    bridge.stop();
    EXPECT_FALSE(bridge.isRunning());
}

TEST(ZMQBridgeTest, DoubleInitializeReturnsFalse) {
    ZMQBridge bridge;
    ZMQConfig cfg;
    cfg.bind_address = "tcp://127.0.0.1:" + std::to_string(nextPort());
    EXPECT_TRUE(bridge.initialize(cfg));
    EXPECT_FALSE(bridge.initialize(cfg));
    bridge.stop();
}

TEST(ZMQBridgeTest, DoubleStopIsSafe) {
    ZMQBridge bridge;
    ZMQConfig cfg;
    cfg.bind_address = "tcp://127.0.0.1:" + std::to_string(nextPort());
    bridge.initialize(cfg);
    bridge.stop();
    bridge.stop();
    EXPECT_FALSE(bridge.isRunning());
}

TEST(ZMQBridgeTest, DestructorStopsBridge) {
    ZMQBridge bridge;
    ZMQConfig cfg;
    cfg.bind_address = "tcp://127.0.0.1:" + std::to_string(nextPort());
    bridge.initialize(cfg);
    // bridge goes out of scope, destructor calls stop()
}

// ============================================================================
// 2. Publish
// ============================================================================

TEST(ZMQBridgeTest, PublishTickIncrementsCounter) {
    ZMQBridge bridge;
    ZMQConfig cfg;
    cfg.bind_address = "tcp://127.0.0.1:" + std::to_string(nextPort());
    ASSERT_TRUE(bridge.initialize(cfg));

    EXPECT_TRUE(bridge.publishTick(makeTick(1000, 100.0)));
    EXPECT_TRUE(bridge.publishTick(makeTick(2000, 101.0)));

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_EQ(bridge.ticksPublished(), 2u);
    EXPECT_EQ(bridge.droppedCount(), 0u);
    bridge.stop();
}

TEST(ZMQBridgeTest, PublishOrderIncrementsCounter) {
    ZMQBridge bridge;
    ZMQConfig cfg;
    cfg.bind_address = "tcp://127.0.0.1:" + std::to_string(nextPort());
    ASSERT_TRUE(bridge.initialize(cfg));

    bridge.publishOrder(makeOrder(1));
    bridge.publishOrder(makeOrder(2));

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(bridge.ordersPublished(), 2u);
    bridge.stop();
}

TEST(ZMQBridgeTest, PublishFillIncrementsCounter) {
    ZMQBridge bridge;
    ZMQConfig cfg;
    cfg.bind_address = "tcp://127.0.0.1:" + std::to_string(nextPort());
    ASSERT_TRUE(bridge.initialize(cfg));

    bridge.publishFill(makeFill(10));
    bridge.publishFill(makeFill(20));
    bridge.publishFill(makeFill(30));

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(bridge.fillsPublished(), 3u);
    bridge.stop();
}

// ============================================================================
// 3. ZMQ Socket — subscriber receives messages
// ============================================================================

TEST(ZMQBridgeTest, SubscriberReceivesTick) {
    int port = nextPort();
    ZMQBridge bridge;
    ZMQConfig cfg;
    cfg.bind_address = "tcp://127.0.0.1:" + std::to_string(port);
    ASSERT_TRUE(bridge.initialize(cfg));

    void* ctx = zmq_ctx_new();
    void* sub = zmq_socket(ctx, ZMQ_SUB);
    int timeout = 500;
    zmq_setsockopt(sub, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
    zmq_setsockopt(sub, ZMQ_SUBSCRIBE, "TICK", 4);
    int rc = zmq_connect(sub, ("tcp://127.0.0.1:" + std::to_string(port)).c_str());
    ASSERT_EQ(rc, 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    Tick sent = makeTick(12345678, 99.5);
    bridge.publishTick(sent);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    char buf[128];
    int n = zmq_recv(sub, buf, sizeof(buf), 0);
    ASSERT_GT(n, 0);
    ASSERT_GE(static_cast<size_t>(n), 4 + sizeof(Tick));

    EXPECT_EQ(std::memcmp(buf, "TICK", 4), 0);

    Tick received;
    std::memcpy(&received, buf + 4, sizeof(Tick));
    EXPECT_EQ(received.exchange_timestamp_us, 12345678ULL);
    EXPECT_NEAR(toDouble(received.price), 99.5, 1e-9);

    zmq_close(sub);
    zmq_ctx_destroy(ctx);
    bridge.stop();
}

TEST(ZMQBridgeTest, SubscriberReceivesOrder) {
    int port = nextPort();
    ZMQBridge bridge;
    ZMQConfig cfg;
    cfg.bind_address = "tcp://127.0.0.1:" + std::to_string(port);
    ASSERT_TRUE(bridge.initialize(cfg));

    void* ctx = zmq_ctx_new();
    void* sub = zmq_socket(ctx, ZMQ_SUB);
    int timeout = 500;
    zmq_setsockopt(sub, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
    zmq_setsockopt(sub, ZMQ_SUBSCRIBE, "ORDR", 4);
    zmq_connect(sub, ("tcp://127.0.0.1:" + std::to_string(port)).c_str());

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    OrderRequest sent;
    sent.order_id = 777;
    sent.price = toDecimal(250.0);
    sent.quantity = toDecimal(0.25);
    sent.side = OrderSide::SELL;
    bridge.publishOrder(sent);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    char buf[256];
    int n = zmq_recv(sub, buf, sizeof(buf), 0);
    ASSERT_GT(n, 0);

    EXPECT_EQ(std::memcmp(buf, "ORDR", 4), 0);
    OrderRequest received;
    std::memcpy(&received, buf + 4, sizeof(OrderRequest));
    EXPECT_EQ(received.order_id, 777ULL);
    EXPECT_EQ(received.side, OrderSide::SELL);

    zmq_close(sub);
    zmq_ctx_destroy(ctx);
    bridge.stop();
}

TEST(ZMQBridgeTest, SubscriberReceivesFill) {
    int port = nextPort();
    ZMQBridge bridge;
    ZMQConfig cfg;
    cfg.bind_address = "tcp://127.0.0.1:" + std::to_string(port);
    ASSERT_TRUE(bridge.initialize(cfg));

    void* ctx = zmq_ctx_new();
    void* sub = zmq_socket(ctx, ZMQ_SUB);
    int timeout = 500;
    zmq_setsockopt(sub, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
    zmq_setsockopt(sub, ZMQ_SUBSCRIBE, "FILL", 4);
    zmq_connect(sub, ("tcp://127.0.0.1:" + std::to_string(port)).c_str());

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    Fill sent;
    sent.order_id = 42;
    sent.execution_id = 420;
    sent.fill_price = toDecimal(75.0);
    sent.fill_quantity = toDecimal(0.5);
    bridge.publishFill(sent);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    char buf[256];
    int n = zmq_recv(sub, buf, sizeof(buf), 0);
    ASSERT_GT(n, 0);

    EXPECT_EQ(std::memcmp(buf, "FILL", 4), 0);
    Fill received;
    std::memcpy(&received, buf + 4, sizeof(Fill));
    EXPECT_EQ(received.order_id, 42ULL);
    EXPECT_EQ(received.execution_id, 420ULL);

    zmq_close(sub);
    zmq_ctx_destroy(ctx);
    bridge.stop();
}

// ============================================================================
// 4. Topic Routing
// ============================================================================

TEST(ZMQBridgeTest, SubscriberTopicFilter) {
    int port = nextPort();
    ZMQBridge bridge;
    ZMQConfig cfg;
    cfg.bind_address = "tcp://127.0.0.1:" + std::to_string(port);
    ASSERT_TRUE(bridge.initialize(cfg));

    void* ctx = zmq_ctx_new();
    void* sub = zmq_socket(ctx, ZMQ_SUB);
    int timeout = 300;
    zmq_setsockopt(sub, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
    zmq_setsockopt(sub, ZMQ_SUBSCRIBE, "TICK", 4);
    zmq_connect(sub, ("tcp://127.0.0.1:" + std::to_string(port)).c_str());

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    bridge.publishOrder(makeOrder(1));
    bridge.publishFill(makeFill(1));
    bridge.publishTick(makeTick(9999, 50.0));

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    char buf[256];
    int n = zmq_recv(sub, buf, sizeof(buf), 0);
    ASSERT_GT(n, 0);
    EXPECT_EQ(std::memcmp(buf, "TICK", 4), 0);

    n = zmq_recv(sub, buf, sizeof(buf), 0);
    EXPECT_LT(n, 0);

    zmq_close(sub);
    zmq_ctx_destroy(ctx);
    bridge.stop();
}

// ============================================================================
// 5. Statistics
// ============================================================================

TEST(ZMQBridgeTest, CountersInitiallyZero) {
    ZMQBridge bridge;
    EXPECT_EQ(bridge.ticksPublished(), 0u);
    EXPECT_EQ(bridge.ordersPublished(), 0u);
    EXPECT_EQ(bridge.fillsPublished(), 0u);
    EXPECT_EQ(bridge.droppedCount(), 0u);
}

TEST(ZMQBridgeTest, StatsAfterStop) {
    ZMQBridge bridge;
    ZMQConfig cfg;
    cfg.bind_address = "tcp://127.0.0.1:" + std::to_string(nextPort());
    ASSERT_TRUE(bridge.initialize(cfg));

    bridge.publishTick(makeTick(1, 10.0));
    bridge.publishTick(makeTick(2, 20.0));
    bridge.publishOrder(makeOrder(1));

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    bridge.stop();

    EXPECT_EQ(bridge.ticksPublished(), 2u);
    EXPECT_EQ(bridge.ordersPublished(), 1u);
    EXPECT_EQ(bridge.fillsPublished(), 0u);
    EXPECT_EQ(bridge.droppedCount(), 0u);
}

// ============================================================================
// 6. Thread Safety
// ============================================================================

TEST(ZMQBridgeTest, ConcurrentPublishes) {
    ZMQBridge bridge;
    ZMQConfig cfg;
    cfg.bind_address = "tcp://127.0.0.1:" + std::to_string(nextPort());
    ASSERT_TRUE(bridge.initialize(cfg));

    constexpr int N = 500;
    std::thread t1([&] {
        for (int i = 0; i < N; ++i) {
            bridge.publishTick(makeTick(1000 + i, 100.0 + i * 0.1));
        }
    });
    std::thread t2([&] {
        for (int i = 0; i < N; ++i) {
            bridge.publishOrder(makeOrder(100 + i));
        }
    });
    std::thread t3([&] {
        for (int i = 0; i < N; ++i) {
            bridge.publishFill(makeFill(200 + i));
        }
    });

    t1.join();
    t2.join();
    t3.join();

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    EXPECT_EQ(bridge.ticksPublished(), static_cast<uint64_t>(N));
    EXPECT_EQ(bridge.ordersPublished(), static_cast<uint64_t>(N));
    EXPECT_EQ(bridge.fillsPublished(), static_cast<uint64_t>(N));

    bridge.stop();
}
