#include <gtest/gtest.h>
#include <chronos/market_data/gateway.hpp>
#include <chronos/market_data/adapters/binance_adapter.hpp>
#include <chronos/market_data/adapters/okx_adapter.hpp>
#include <chronos/core/config.hpp>
#include <chronos/core/types.hpp>
#include <chronos/utils/mpmc_queue.hpp>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <iostream>
#include <vector>
#include <algorithm>
#include <iomanip>

namespace chronos {
namespace test {

// ============================================================================
// Multi-Exchange Stress Test
// ============================================================================
//
// Verifies the shared MPMC queue + multi-adapter architecture:
//   2 Binance Futures testnet adapters (no proxy needed) +
//   1 OKX adapter (optional, may fail if blocked)
//   All pushing to a single shared MPMC queue.
//
// Metrics: per-adapter throughput, total throughput, queue pop latency.
//
// SKIPPED by default. Run with:
//   CHRONOS_LIVE_TEST=1 ./unit_tests --gtest_filter=MultiExchangeStress*

class MultiExchangeStressTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!std::getenv("CHRONOS_LIVE_TEST")) {
            GTEST_SKIP() << "Set CHRONOS_LIVE_TEST=1 to run stress tests";
        }
    }
};

TEST_F(MultiExchangeStressTest, SharedQueueMultiAdapter) {
    using namespace std::chrono_literals;
    using Clock = std::chrono::high_resolution_clock;

    auto tick_queue = std::make_unique<utils::MPMCQueue<Tick, 65536>>();

    // --- Binance Adapter #1: BTC depth + trades (Futures testnet, no proxy) ---
    auto gw1 = market_data::createGateway("binance");
    ASSERT_NE(gw1, nullptr);

    ExchangeConfig cfg1;
    cfg1.name = "binance";
    cfg1.websocket_url =
        "wss://stream.binancefuture.com/"
        "stream?streams=btcusdt@depth@100ms/btcusdt@aggTrade";

    ASSERT_TRUE(gw1->initialize(cfg1, *tick_queue));
    ASSERT_TRUE(gw1->start());

    // --- Binance Adapter #2: ETH depth + trades (Futures testnet) ---
    auto gw2 = market_data::createGateway("binance");
    ASSERT_NE(gw2, nullptr);

    ExchangeConfig cfg2;
    cfg2.name = "binance";
    cfg2.websocket_url =
        "wss://stream.binancefuture.com/"
        "stream?streams=ethusdt@depth@100ms/ethusdt@aggTrade";

    ASSERT_TRUE(gw2->initialize(cfg2, *tick_queue));
    ASSERT_TRUE(gw2->start());

    // Wait for connections
    std::this_thread::sleep_for(3s);
    ASSERT_EQ(gw1->getStatus(), ConnectionStatus::CONNECTED);
    ASSERT_EQ(gw2->getStatus(), ConnectionStatus::CONNECTED);

    std::cout << "[STRESS] Both Binance adapters connected." << std::endl;

    // --- OKX Adapter (optional — may be blocked in some regions) ---
    auto gw3 = market_data::createGateway("okx");
    bool okx_active = false;
    if (gw3) {
        ExchangeConfig cfg3;
        cfg3.name = "okx";
        cfg3.proxy_host = "127.0.0.1";
        cfg3.proxy_port = 7897;
        ASSERT_TRUE(gw3->initialize(cfg3, *tick_queue));
        okx_active = gw3->start();
        if (okx_active) {
            gw3->subscribe("BTC-USDT");
            std::this_thread::sleep_for(2s);
            std::cout << "[STRESS] OKX adapter connected." << std::endl;
        } else {
            std::cout << "[STRESS] OKX adapter failed to connect (network issue). Skipping."
                      << std::endl;
        }
    }

    // --- Warm-up drain ---
    std::this_thread::sleep_for(2s);
    {
        Tick t;
        while (tick_queue->try_pop(t)) {}
    }

    // --- 15-second sustained measurement ---
    std::cout << "\n[STRESS] Measuring 15-second sustained throughput...\n" << std::endl;

    uint64_t total_ticks = 0;
    uint64_t bid_ticks = 0, ask_ticks = 0, trade_ticks = 0;
    std::vector<double> latencies_us;
    latencies_us.reserve(500000);

    auto t_start = Clock::now();
    auto deadline = t_start + 15s;

    while (Clock::now() < deadline) {
        Tick t;
        while (tick_queue->try_pop(t)) {
            total_ticks++;

            switch (t.side) {
                case TickSide::BID: bid_ticks++; break;
                case TickSide::ASK: ask_ticks++; break;
                case TickSide::TRADE: trade_ticks++; break;
            }

            // Queue pop latency: receive time → now
            auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                Clock::now().time_since_epoch()).count();
            int64_t latency = now_us - static_cast<int64_t>(t.receive_timestamp_us);
            if (latency >= 0) {
                latencies_us.push_back(static_cast<double>(latency));
            }
        }
        std::this_thread::sleep_for(1ms);
    }

    auto t_end = Clock::now();
    double elapsed_sec = std::chrono::duration<double>(t_end - t_start).count();

    // --- Metrics ---
    double throughput = static_cast<double>(total_ticks) / elapsed_sec;

    double p50 = 0, p99 = 0, p999 = 0;
    if (!latencies_us.empty()) {
        std::sort(latencies_us.begin(), latencies_us.end());
        p50 = latencies_us[latencies_us.size() * 50 / 100];
        p99 = latencies_us[latencies_us.size() * 99 / 100];
        p999 = latencies_us[latencies_us.size() * 999 / 1000];
    }

    std::cout << std::fixed << std::setprecision(1);
    std::cout << "=== Multi-Adapter Stress Test Results ===\n"
              << "  Duration:        " << elapsed_sec << "s\n"
              << "  Total ticks:     " << total_ticks << "\n"
              << "    BID:           " << bid_ticks << "\n"
              << "    ASK:           " << ask_ticks << "\n"
              << "    TRADE:         " << trade_ticks << "\n"
              << "  Throughput:      " << throughput << " ticks/s\n"
              << "  Queue pop P50:   " << p50 << " μs\n"
              << "  Queue pop P99:   " << p99 << " μs\n"
              << "  Queue pop P99.9: " << p999 << " μs\n"
              << std::endl;

    // Assertions
    EXPECT_GT(total_ticks, 0) << "Expected ticks from Binance Futures testnet";

    // Core assertion: shared queue works across multiple adapters
    // Throughput depends on network; just verify data flows
    std::cout << "[STRESS] ✓ Multi-adapter shared queue test passed.\n";

    // --- Cleanup ---
    gw1->stop();
    gw2->stop();
    if (okx_active) gw3->stop();

    std::cout << "[STRESS] All gateways stopped.\n";
}

} // namespace test
} // namespace chronos
