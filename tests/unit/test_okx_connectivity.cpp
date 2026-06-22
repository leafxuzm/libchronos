#include <gtest/gtest.h>
#include <chronos/market_data/gateway.hpp>
#include <chronos/market_data/adapters/okx_adapter.hpp>
#include <chronos/core/config.hpp>
#include <chronos/core/types.hpp>
#include <chronos/utils/mpmc_queue.hpp>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <iostream>

namespace chronos {
namespace test {

// ============================================================================
// OKX Public WebSocket Connectivity Test
// ============================================================================
//
// Connects to OKX public WebSocket, subscribes to BTC-USDT books5 + trades,
// and verifies tick data is received.
//
// SKIPPED by default. Run with:
//   CHRONOS_LIVE_TEST=1 ./unit_tests --gtest_filter=OKXConnectivity*
//
// IMPORTANT: This test uses OKX PUBLIC market data only.
// No real trading, no API keys required.

class OKXConnectivityTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!std::getenv("CHRONOS_LIVE_TEST")) {
            GTEST_SKIP() << "Set CHRONOS_LIVE_TEST=1 to run connectivity tests";
        }
    }
};

TEST_F(OKXConnectivityTest, ConnectAndReceiveTicks) {
    using namespace std::chrono_literals;

    auto tick_queue = std::make_unique<utils::MPMCQueue<Tick, 65536>>();

    auto gateway = market_data::createGateway("okx");
    ASSERT_NE(gateway, nullptr) << "Failed to create OkxAdapter";

    ExchangeConfig config;
    config.name = "okx";
    config.symbols = {"btc-usdt"};
    // Use proxy for access from mainland China
    config.proxy_host = "127.0.0.1";
    config.proxy_port = 7897;

    ASSERT_TRUE(gateway->initialize(config, *tick_queue))
        << "Failed to initialize OkxAdapter";

    std::cout << "[TEST] Connecting to OKX Public WebSocket..." << std::endl;
    ASSERT_TRUE(gateway->start()) << "Failed to start OkxAdapter";

    std::cout << "[TEST] Connected! Subscribing to BTC-USDT..." << std::endl;
    std::this_thread::sleep_for(2s);
    ASSERT_TRUE(gateway->subscribe("btc-usdt"));

    std::cout << "[TEST] Draining ticks for 8 seconds..." << std::endl;
    std::this_thread::sleep_for(8s);

    // Drain queue and count
    Tick t;
    int bid = 0, ask = 0, trade = 0;
    while (tick_queue->try_pop(t)) {
        switch (t.side) {
            case TickSide::BID: bid++; break;
            case TickSide::ASK: ask++; break;
            case TickSide::TRADE: trade++; break;
        }
    }
    int total = bid + ask + trade;
    std::cout << "[TEST] Ticks received: " << total
              << " (BID=" << bid << " ASK=" << ask << " TRADE=" << trade << ")" << std::endl;

    EXPECT_GT(total, 0) << "Expected at least some ticks from OKX public feed";

    gateway->stop();
    std::cout << "[TEST] Gateway stopped. Test PASSED." << std::endl;
}

} // namespace test
} // namespace chronos
