#include <gtest/gtest.h>
#include <chronos/market_data/any_gateway.hpp>
#include <chronos/market_data/adapters/thin_mux_adapter.hpp>
#include <chronos/market_data/gateway_v2_factory.hpp>
#include <chronos/io/transports/transport.hpp>
#include <chronos/io/protocols/protocol.hpp>
#include <chronos/core/types.hpp>
#include <chronos/core/config.hpp>
#include <chronos/utils/mpmc_queue.hpp>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <thread>
#include <algorithm>
#include <cctype>

namespace chronos {
namespace test {

// ============================================================================
// Mock Transport
// ============================================================================
class MockTransport final : public market_data::Transport {
public:
    bool connect(const ExchangeConfig&) override {
        connected_ = true;
        connect_called_ = true;
        return true;
    }
    void disconnect() override {
        connected_ = false;
        cv_.notify_all();
    }
    bool send(const std::string& msg) override {
        std::lock_guard lock(mutex_);
        sent_.push_back(msg);
        return true;
    }
    bool sendPing() override {
        std::lock_guard lock(mutex_);
        sent_.push_back("__PING__");
        return true;
    }
    std::string receive() override {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] { return !receive_queue_.empty() || !connected_; });
        if (!connected_ || receive_queue_.empty()) return "";
        auto msg = receive_queue_.front();
        receive_queue_.pop();
        return msg;
    }
    bool isConnected() const override { return connected_; }
    void stop() override {
        connected_ = false;
        cv_.notify_all();
    }

    void enqueueReceive(std::string msg) {
        {
            std::lock_guard lock(mutex_);
            receive_queue_.push(std::move(msg));
        }
        cv_.notify_one();
    }
    std::vector<std::string> drainSent() {
        std::lock_guard lock(mutex_);
        auto copy = sent_;
        sent_.clear();
        return copy;
    }
    bool connect_called() const { return connect_called_; }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::string> receive_queue_;
    std::vector<std::string> sent_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> connect_called_{false};
};

// ============================================================================
// Mock Protocol
// ============================================================================
class MockProtocol final : public market_data::Protocol {
public:
    size_t parse(const std::string& msg, uint64_t recv_ts,
                 std::function<void(Tick&&)> onTick,
                 std::function<uint32_t(const std::string&)> resolveId) override {
        std::lock_guard lock(mutex_);
        parsed_.push_back(msg);
        if (should_emit_tick_) {
            Tick t{};
            t.exchange_id = 1;
            t.receive_timestamp_us = recv_ts;
            t.symbol_id = resolveId("TEST");
            t.price = toDecimal(100.0);
            t.quantity = toDecimal(1.0);
            t.side = TickSide::BID;
            onTick(std::move(t));
            ticks_emitted_++;
        }
        return ticks_emitted_ > 0 ? 1 : 0;
    }
    std::string subscribeRequest(const std::vector<std::string>& syms) override {
        std::lock_guard lock(mutex_);
        last_sub_ = syms;
        std::string req = "SUB:";
        for (size_t i = 0; i < syms.size(); ++i) {
            if (i > 0) req += ",";
            req += syms[i];
        }
        return req;
    }
    std::string unsubscribeRequest(const std::vector<std::string>& syms) override {
        std::lock_guard lock(mutex_);
        last_unsub_ = syms;
        std::string req = "UNSUB:";
        for (size_t i = 0; i < syms.size(); ++i) {
            if (i > 0) req += ",";
            req += syms[i];
        }
        return req;
    }
    bool isPingMessage(const std::string&) const override { return false; }
    bool isPongMessage(const std::string&) const override { return false; }
    std::string heartbeatPayload() const override { return ""; }
    bool usesWsPing() const override { return true; }
    std::string normalizeSymbol(const std::string& raw) const override {
        std::string n = raw;
        std::transform(n.begin(), n.end(), n.begin(), ::toupper);
        return n;
    }
    std::string defaultUrl() const override { return "wss://mock.example.com/ws"; }

    void setEmitTick(bool v) { should_emit_tick_ = v; }
    int ticksEmitted() const { return ticks_emitted_; }
    std::vector<std::string> drainParsed() {
        std::lock_guard lock(mutex_);
        auto copy = parsed_;
        parsed_.clear();
        return copy;
    }
    std::vector<std::string> lastSubscribed() const {
        std::lock_guard lock(mutex_);
        return last_sub_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> parsed_;
    std::vector<std::string> last_sub_;
    std::vector<std::string> last_unsub_;
    bool should_emit_tick_ = true;
    int ticks_emitted_ = 0;
};

// ============================================================================
// Fixture
// ============================================================================
class AnyGatewayTest : public ::testing::Test {
protected:
    void SetUp() override {
        queue_ = std::make_unique<utils::MPMCQueue<Tick, 65536>>();
    }

    market_data::AnyGateway makeGateway(bool init = true) {
        auto transport = std::make_unique<MockTransport>();
        auto protocol  = std::make_unique<MockProtocol>();
        transport_ = transport.get();
        protocol_  = protocol.get();

        market_data::ThinMuxAdapter adapter(std::move(transport), std::move(protocol));
        if (init) {
            ExchangeConfig cfg;
            cfg.name = "mock";
            cfg.heartbeat_interval_ms = 1000;
            adapter.initialize(cfg, *queue_);
        }
        return market_data::AnyGateway(std::move(adapter));
    }

    std::unique_ptr<utils::MPMCQueue<Tick, 65536>> queue_;
    MockTransport* transport_ = nullptr;
    MockProtocol*  protocol_  = nullptr;
};

// ============================================================================
// 1. Factory
// ============================================================================
TEST_F(AnyGatewayTest, FactoryKnownNames) {
    ExchangeConfig cfg;
    auto gw = market_data::createGatewayV2("binance", cfg, *queue_);
    EXPECT_TRUE(static_cast<bool>(gw));
    auto gw2 = market_data::createGatewayV2("okx", cfg, *queue_);
    EXPECT_TRUE(static_cast<bool>(gw2));
}

TEST_F(AnyGatewayTest, FactoryUnknownNameReturnsEmpty) {
    ExchangeConfig cfg;
    auto gw = market_data::createGatewayV2("nonexistent", cfg, *queue_);
    EXPECT_FALSE(static_cast<bool>(gw));
}

// ============================================================================
// 2. Lifecycle
// ============================================================================
TEST_F(AnyGatewayTest, StartStopLifecycle) {
    auto gw = makeGateway();
    ASSERT_TRUE(gw.start());
    EXPECT_TRUE(gw.isRunning());
    gw.stop();
    EXPECT_FALSE(gw.isRunning());
}

TEST_F(AnyGatewayTest, TransportConnectCalledOnStart) {
    auto gw = makeGateway();
    ASSERT_TRUE(gw.start());
    EXPECT_TRUE(transport_->connect_called());
    gw.stop();
}

// ============================================================================
// 3. Subscribe / Unsubscribe
// ============================================================================
TEST_F(AnyGatewayTest, SubscribeSendsRequest) {
    auto gw = makeGateway();
    ASSERT_TRUE(gw.subscribe("btcusdt"));
    auto sent = transport_->drainSent();
    ASSERT_GE(sent.size(), 1u);
    EXPECT_EQ(sent[0], "SUB:BTCUSDT");
}

TEST_F(AnyGatewayTest, SubscribeDedup) {
    auto gw = makeGateway();
    ASSERT_TRUE(gw.subscribe("btcusdt"));
    transport_->drainSent();
    ASSERT_TRUE(gw.subscribe("btcusdt"));
    auto sent = transport_->drainSent();
    EXPECT_TRUE(sent.empty());
}

TEST_F(AnyGatewayTest, UnsubscribeSendsRequest) {
    auto gw = makeGateway();
    gw.subscribe("btcusdt");
    transport_->drainSent();
    ASSERT_TRUE(gw.unsubscribe("btcusdt"));
    auto sent = transport_->drainSent();
    ASSERT_GE(sent.size(), 1u);
    EXPECT_EQ(sent[0], "UNSUB:BTCUSDT");
}

// ============================================================================
// 4. Tick flow
// ============================================================================
TEST_F(AnyGatewayTest, ReadLoopPushesTicks) {
    auto gw = makeGateway();
    ASSERT_TRUE(gw.subscribe("test"));  // pre-register symbol for resolveId
    transport_->drainSent();

    transport_->enqueueReceive(R"({"data":"test"})");
    ASSERT_TRUE(gw.start());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    gw.stop();

    EXPECT_GT(protocol_->ticksEmitted(), 0);
    Tick t;
    bool got = queue_->try_pop(t);
    EXPECT_TRUE(got);
    if (got) {
        EXPECT_NE(t.symbol_id, 0u);
        EXPECT_EQ(t.side, TickSide::BID);
    }
}

// ============================================================================
// 5. Statistics
// ============================================================================
TEST_F(AnyGatewayTest, StatisticsInitialState) {
    auto gw = makeGateway();
    auto stats = gw.getStatistics();
    EXPECT_EQ(stats.messages_received, 0u);
    EXPECT_EQ(stats.reconnections, 0u);
}

// ============================================================================
// 6. Resubscribe on start
// ============================================================================
TEST_F(AnyGatewayTest, ResubscribeOnStart) {
    auto gw = makeGateway();
    gw.subscribe("btcusdt");
    gw.subscribe("ethusdt");
    transport_->drainSent();

    ASSERT_TRUE(gw.start());
    auto sent = transport_->drainSent();

    bool found = false;
    for (const auto& s : sent) {
        if (s.find("ETHUSDT") != std::string::npos &&
            s.find("BTCUSDT") != std::string::npos) {
            found = true;
        }
    }
    EXPECT_TRUE(found) << "Expected batch resubscribe with tracked symbols";
    gw.stop();
}

// ============================================================================
// 7. Heartbeat
// ============================================================================
TEST_F(AnyGatewayTest, HeartbeatUsesWsPing) {
    auto gw = makeGateway();
    ASSERT_TRUE(gw.start());

    std::this_thread::sleep_for(std::chrono::milliseconds(1200));

    auto sent = transport_->drainSent();
    bool found = false;
    for (const auto& s : sent) {
        if (s == "__PING__") { found = true; break; }
    }
    EXPECT_TRUE(found);
    gw.stop();
}

// ============================================================================
// 8. Compile-time concept
// ============================================================================
TEST_F(AnyGatewayTest, StaticAssertVerified) {
    // static_assert(ExchangeAdapter<ThinMuxAdapter>) in thin_mux_adapter.cpp
    EXPECT_TRUE(true);
}

} // namespace test
} // namespace chronos
