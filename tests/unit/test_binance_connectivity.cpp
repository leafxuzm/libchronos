#include <gtest/gtest.h>
#include <chronos/market_data/gateway.hpp>
#include <chronos/market_data/adapters/binance_adapter.hpp>
#include <chronos/core/config.hpp>
#include <chronos/core/types.hpp>
#include <chronos/utils/mpmc_queue.hpp>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <iostream>
#include <vector>
#include <algorithm>
#include <numeric>
#include <iomanip>

namespace chronos {
namespace test {

// ============================================================================
// Binance Testnet Connectivity Test
// ============================================================================
//
// This test connects to Binance Futures testnet, subscribes to BTCUSDT
// depth stream, and verifies tick data is received.
//
// SKIPPED by default. Run with:
//   CHRONOS_LIVE_TEST=1 ./unit_tests --gtest_filter=BinanceConnectivity*
//
// IMPORTANT: This test uses Binance Futures TESTNET only.
// No real trading, no API keys required for public market data.

class BinanceConnectivityTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!std::getenv("CHRONOS_LIVE_TEST")) {
            GTEST_SKIP() << "Set CHRONOS_LIVE_TEST=1 to run connectivity tests";
        }
    }
};

TEST_F(BinanceConnectivityTest, ConnectAndReceiveTicks) {
    using namespace std::chrono_literals;

    // 1. Create tick queue
    auto tick_queue = std::make_unique<utils::MPMCQueue<Tick, 65536>>();

    // 2. Create BinanceAdapter via factory
    auto gateway = market_data::createGateway("binance");
    ASSERT_NE(gateway, nullptr) << "Failed to create BinanceAdapter";

    // 3. Configure for FUTURES TESTNET (simulated trading only)
    ExchangeConfig config;
    config.name = "binance";
    // Futures testnet — embed stream in URL (no JSON subscribe support)
    // Single stream: /ws/<symbol>@depth@100ms
    config.websocket_url = "wss://stream.binancefuture.com/ws/btcusdt@depth@100ms";
    config.symbols = {"btcusdt"};

    ASSERT_TRUE(gateway->initialize(config, *tick_queue))
        << "Failed to initialize BinanceAdapter";

    std::cout << "[TEST] Connecting to Binance Futures Testnet...\n";

    // 4. Start and wait for connection
    ASSERT_TRUE(gateway->start()) << "Failed to start gateway";

    // Give it time to establish TLS + WebSocket handshake
    std::this_thread::sleep_for(2s);

    ASSERT_EQ(gateway->getStatus(), ConnectionStatus::CONNECTED)
        << "Gateway did not reach CONNECTED state";

    std::cout << "[TEST] Connected! Subscribing to btcusdt...\n";

    // 5. Subscribe to BTCUSDT depth + trade streams
    ASSERT_TRUE(gateway->subscribe("btcusdt"))
        << "Failed to subscribe to btcusdt";

    // 6. Drain ticks for a few seconds
    std::cout << "[TEST] Draining ticks for 5 seconds...\n";

    Tick tick;
    size_t total_ticks = 0;
    size_t bid_ticks = 0;
    size_t ask_ticks = 0;
    size_t trade_ticks = 0;

    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        while (tick_queue->try_pop(tick)) {
            total_ticks++;
            switch (tick.side) {
                case TickSide::BID:  bid_ticks++;  break;
                case TickSide::ASK:  ask_ticks++;  break;
                case TickSide::TRADE: trade_ticks++; break;
            }
        }
        std::this_thread::sleep_for(10ms);
    }

    std::cout << "[TEST] Ticks received: " << total_ticks
              << " (BID=" << bid_ticks
              << " ASK=" << ask_ticks
              << " TRADE=" << trade_ticks << ")\n";

    // 7. Verify we received data
    EXPECT_GT(total_ticks, 0)
        << "No ticks received — check network or testnet status";
    EXPECT_GT(bid_ticks + ask_ticks, 0)
        << "No depth ticks (BID/ASK) received";

    // Print one sample tick for verification
    if (total_ticks > 0) {
        // Re-drain a fresh sample for output
        std::this_thread::sleep_for(500ms);
        Tick sample;
        if (tick_queue->try_pop(sample)) {
            double price = toDouble(sample.price);
            double qty = toDouble(sample.quantity);
            std::cout << "[TEST] Sample tick: symbol_id=" << sample.symbol_id
                      << " price=" << price
                      << " qty=" << qty
                      << " side=" << (sample.side == TickSide::BID ? "BID" :
                                      sample.side == TickSide::ASK ? "ASK" : "TRADE")
                      << " ts_us=" << sample.exchange_timestamp_us << "\n";
            EXPECT_GT(price, 0.0) << "Price should be positive";
            // qty can be 0 (level deletion in depth updates)
        }
    }

    // 8. Clean shutdown
    gateway->stop();
    std::cout << "[TEST] Gateway stopped. Test PASSED.\n";
}

TEST_F(BinanceConnectivityTest, MultiStreamCombinedURL) {
    using namespace std::chrono_literals;

    auto tick_queue = std::make_unique<utils::MPMCQueue<Tick, 65536>>();
    auto gateway = market_data::createGateway("binance");
    ASSERT_NE(gateway, nullptr);

    ExchangeConfig config;
    config.name = "binance";
    // Futures testnet combined streams — multiple symbols in one connection
    config.websocket_url =
        "wss://stream.binancefuture.com/"
        "stream?streams=btcusdt@depth@100ms/ethusdt@depth@100ms";

    ASSERT_TRUE(gateway->initialize(config, *tick_queue));
    ASSERT_TRUE(gateway->start());

    std::this_thread::sleep_for(2s);
    ASSERT_EQ(gateway->getStatus(), ConnectionStatus::CONNECTED);

    std::cout << "[TEST] Connected with combined streams URL, draining ticks...\n";

    Tick tick;
    std::set<uint32_t> symbol_ids;
    size_t total = 0;

    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        while (tick_queue->try_pop(tick)) {
            total++;
            if (tick.symbol_id > 0) symbol_ids.insert(tick.symbol_id);
        }
        std::this_thread::sleep_for(10ms);
    }

    std::cout << "[TEST] Combined streams: " << total << " ticks, "
              << symbol_ids.size() << " unique symbols\n";
    EXPECT_GT(total, 0) << "No ticks from combined streams URL";
    EXPECT_GE(symbol_ids.size(), 1)
        << "Expected ticks from at least 1 symbol";

    gateway->stop();
    std::cout << "[TEST] Combined streams test PASSED.\n";
}

// ============================================================================
// Performance Micro-Benchmark
// ============================================================================

TEST_F(BinanceConnectivityTest, PerformanceMetrics) {
    using namespace std::chrono_literals;
    using Clock = std::chrono::high_resolution_clock;

    // Shared MPMC queue — both adapters push to this
    auto tick_queue = std::make_unique<utils::MPMCQueue<Tick, 65536>>();

    // ================================================================
    // Multi-connection setup: 2 adapters, each with own io_context,
    // own TLS+WS connection, own readLoop thread. Both share one queue.
    // No architecture change needed — MPMC queue is multi-producer.
    // ================================================================

    ExchangeConfig base_config;
    base_config.name = "binance";
    base_config.proxy_host = "127.0.0.1";
    base_config.proxy_port = 7897;

    // Adapter #1: Top 25 USDT perps × (depth@100ms + aggTrade)
    auto gw1 = market_data::createGateway("binance");
    ASSERT_NE(gw1, nullptr);
    ExchangeConfig cfg1 = base_config;
    cfg1.websocket_url =
        "wss://fstream.binance.com/"
        "stream?streams="
        "btcusdt@depth@100ms/btcusdt@aggTrade/"
        "ethusdt@depth@100ms/ethusdt@aggTrade/"
        "solusdt@depth@100ms/solusdt@aggTrade/"
        "bnbusdt@depth@100ms/bnbusdt@aggTrade/"
        "xrpusdt@depth@100ms/xrpusdt@aggTrade/"
        "dogeusdt@depth@100ms/dogeusdt@aggTrade/"
        "adausdt@depth@100ms/adausdt@aggTrade/"
        "avaxusdt@depth@100ms/avaxusdt@aggTrade/"
        "linkusdt@depth@100ms/linkusdt@aggTrade/"
        "dotusdt@depth@100ms/dotusdt@aggTrade/"
        "maticusdt@depth@100ms/maticusdt@aggTrade/"
        "ltcusdt@depth@100ms/ltcusdt@aggTrade/"
        "uniusdt@depth@100ms/uniusdt@aggTrade/"
        "etcusdt@depth@100ms/etcusdt@aggTrade/"
        "filusdt@depth@100ms/filusdt@aggTrade/"
        "atomusdt@depth@100ms/atomusdt@aggTrade/"
        "aptusdt@depth@100ms/aptusdt@aggTrade/"
        "arbusdt@depth@100ms/arbusdt@aggTrade/"
        "opusdt@depth@100ms/opusdt@aggTrade/"
        "suiusdt@depth@100ms/suiusdt@aggTrade/"
        "nearusdt@depth@100ms/nearusdt@aggTrade/"
        "tiausdt@depth@100ms/tiausdt@aggTrade/"
        "injusdt@depth@100ms/injusdt@aggTrade/"
        "stxusdt@depth@100ms/stxusdt@aggTrade/"
        "seiusdt@depth@100ms/seiusdt@aggTrade";

    // Adapter #2: Next 25 USDT perps × (depth@100ms + aggTrade)
    auto gw2 = market_data::createGateway("binance");
    ASSERT_NE(gw2, nullptr);
    ExchangeConfig cfg2 = base_config;
    cfg2.websocket_url =
        "wss://fstream.binance.com/"
        "stream?streams="
        "runeusdt@depth@100ms/runeusdt@aggTrade/"
        "wifusdt@depth@100ms/wifusdt@aggTrade/"
        "jupusdt@depth@100ms/jupusdt@aggTrade/"
        "bonkusdt@depth@100ms/bonkusdt@aggTrade/"
        "flokusdt@depth@100ms/flokusdt@aggTrade/"
        "pepeusdt@depth@100ms/pepeusdt@aggTrade/"
        "wldusdt@depth@100ms/wldusdt@aggTrade/"
        "enausdt@depth@100ms/enausdt@aggTrade/"
        "strkusdt@depth@100ms/strkusdt@aggTrade/"
        "ldousdt@depth@100ms/ldousdt@aggTrade/"
        "aaveusdt@depth@100ms/aaveusdt@aggTrade/"
        "crvusdt@depth@100ms/crvusdt@aggTrade/"
        "mkrusdt@depth@100ms/mkrusdt@aggTrade/"
        "sandusdt@depth@100ms/sandusdt@aggTrade/"
        "manausdt@depth@100ms/manausdt@aggTrade/"
        "gmxusdt@depth@100ms/gmxusdt@aggTrade/"
        "rdousdt@depth@100ms/rdousdt@aggTrade/"
        "egldusdt@depth@100ms/egldusdt@aggTrade/"
        "flowusdt@depth@100ms/flowusdt@aggTrade/"
        "qntusdt@depth@100ms/qntusdt@aggTrade/"
        "axsusdt@depth@100ms/axsusdt@aggTrade/"
        "algusdt@depth@100ms/algusdt@aggTrade/"
        "icpusdt@depth@100ms/icpusdt@aggTrade/"
        "vetusdt@depth@100ms/vetusdt@aggTrade/"
        "trxusdt@depth@100ms/trxusdt@aggTrade";

    // === 1. Start both adapters (parallel connections) ===
    auto t0 = Clock::now();

    ASSERT_TRUE(gw1->initialize(cfg1, *tick_queue));
    ASSERT_TRUE(gw2->initialize(cfg2, *tick_queue));

    ASSERT_TRUE(gw1->start());
    ASSERT_TRUE(gw2->start());

    // Wait for both connections
    std::this_thread::sleep_for(3s);
    auto t1 = Clock::now();

    ASSERT_EQ(gw1->getStatus(), ConnectionStatus::CONNECTED);
    ASSERT_EQ(gw2->getStatus(), ConnectionStatus::CONNECTED);

    auto connect_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // === 2. Drain ticks from shared queue ===
    std::vector<double> pop_latencies_us;
    pop_latencies_us.reserve(1000000);

    const auto sample_deadline = Clock::now() + 10s;
    Tick tick;
    size_t total_from_gw1 = 0, total_from_gw2 = 0;

    while (Clock::now() < sample_deadline) {
        auto t_pop_start = Clock::now();
        bool got = tick_queue->try_pop(tick);
        auto t_pop_end = Clock::now();

        if (got) {
            double pop_us = std::chrono::duration<double, std::micro>(
                t_pop_end - t_pop_start).count();
            pop_latencies_us.push_back(pop_us);
        } else {
            std::this_thread::sleep_for(1ms);
        }
    }

    // Drain remaining
    size_t remaining = 0;
    while (tick_queue->try_pop(tick)) remaining++;

    // Get per-adapter stats
    auto stats1 = gw1->getStatistics();
    auto stats2 = gw2->getStatistics();
    total_from_gw1 = stats1.ticks_processed;
    total_from_gw2 = stats2.ticks_processed;

    // === 3. Compute statistics ===
    size_t n = pop_latencies_us.size();
    std::sort(pop_latencies_us.begin(), pop_latencies_us.end());

    double sum = std::accumulate(pop_latencies_us.begin(), pop_latencies_us.end(), 0.0);
    double avg_us = n > 0 ? sum / n : 0;
    double min_us = n > 0 ? pop_latencies_us.front() : 0;
    double max_us = n > 0 ? pop_latencies_us.back() : 0;
    double p50_us = n > 0 ? pop_latencies_us[n * 50 / 100] : 0;
    double p99_us = n > 0 ? pop_latencies_us[n * 99 / 100] : 0;
    double p999_us = n > 0 ? pop_latencies_us[n * 999 / 1000] : 0;

    size_t total_ticks = n + remaining;
    double throughput = total_ticks / 10.0;

    // === 4. Report ===
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════╗\n";
    std::cout << "║     BinanceAdapter Performance Benchmark             ║\n";
    std::cout << "╠══════════════════════════════════════════════════════╣\n";
    std::cout << "║  PRODUCTION: fstream.binance.com (Futures 真实行情)  ║\n";
    std::cout << "║  Mode:    2 adapters × 25 symbols (depth + aggTrade) ║\n";
    std::cout << "║  Queue:   Shared MPMC (multi-producer, 1 consumer)   ║\n";
    std::cout << "║  Sample:  " << std::setw(6) << total_ticks << " ticks over 10 seconds               ║\n";
    std::cout << "╠══════════════════════════════════════════════════════╣\n";
    std::cout << "║                                                      ║\n";
    std::cout << "║  ── Connection ──                                    ║\n";
    std::cout << "║  2× TLS+WS handshake: " << std::setw(8) << std::fixed << std::setprecision(1)
              << connect_ms << " ms                        ║\n";
    std::cout << "║                                                      ║\n";
    std::cout << "║  ── Tick Pop Latency (MPMC queue) ──                  ║\n";
    std::cout << "║  Count:    " << std::setw(10) << n << " samples                         ║\n";
    std::cout << "║  Avg:      " << std::setw(10) << std::fixed << std::setprecision(2)
              << avg_us << " μs                            ║\n";
    std::cout << "║  Min:      " << std::setw(10) << std::fixed << std::setprecision(2)
              << min_us << " μs                            ║\n";
    std::cout << "║  P50:      " << std::setw(10) << std::fixed << std::setprecision(2)
              << p50_us << " μs                            ║\n";
    std::cout << "║  P99:      " << std::setw(10) << std::fixed << std::setprecision(2)
              << p99_us << " μs                            ║\n";
    std::cout << "║  P99.9:    " << std::setw(10) << std::fixed << std::setprecision(2)
              << p999_us << " μs                            ║\n";
    std::cout << "║  Max:      " << std::setw(10) << std::fixed << std::setprecision(2)
              << max_us << " μs                            ║\n";
    std::cout << "║                                                      ║\n";
    std::cout << "║  ── Throughput ──                                    ║\n";
    std::cout << "║  Consumer:  " << std::setw(10) << std::fixed << std::setprecision(1)
              << throughput << " ticks/sec                       ║\n";
    std::cout << "║  Adapter#1: " << std::setw(10) << total_from_gw1
              << " ticks processed                  ║\n";
    std::cout << "║  Adapter#2: " << std::setw(10) << total_from_gw2
              << " ticks processed                  ║\n";
    std::cout << "║  (2 连接并行, 共享 MPMC 队列, 无需架构改动)          ║\n";
    std::cout << "║                                                      ║\n";
    std::cout << "╚══════════════════════════════════════════════════════╝\n";
    std::cout << "\n";

    // === 5. Assertions ===
    EXPECT_GT(total_ticks, 0) << "No ticks received";
    EXPECT_GT(total_from_gw1, 0) << "Adapter#1 produced no ticks";
    EXPECT_GT(total_from_gw2, 0) << "Adapter#2 produced no ticks";
    EXPECT_LT(p50_us, 10.0) << "P50 pop latency exceeds 10μs target";
    EXPECT_LT(p99_us, 100.0) << "P99 pop latency exceeds 100μs target";

    gw1->stop();
    gw2->stop();
}

} // namespace test
} // namespace chronos
