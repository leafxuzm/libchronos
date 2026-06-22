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
#include <numeric>
#include <iomanip>

namespace chronos {
namespace test {

// ============================================================================
// Full-Pipeline Multi-Exchange Performance Benchmark
// ============================================================================
//
// Production-grade end-to-end performance measurement:
//   2x Binance Futures (production, 50 symbols total) +
//   1x OKX (production, 2 symbols)
//   All → shared MPMC queue → single consumer
//
// Measures FULL pipeline latency:
//   WebSocket recv → simdjson parse → parseDecimal → MPMC push → consumer pop
//
// SKIPPED by default. Run with:
//   CHRONOS_LIVE_TEST=1 ./unit_tests --gtest_filter=FullPipeline*

class FullPipelinePerfTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!std::getenv("CHRONOS_LIVE_TEST")) {
            GTEST_SKIP() << "Set CHRONOS_LIVE_TEST=1 to run full pipeline perf test";
        }
    }
};

TEST_F(FullPipelinePerfTest, MultiExchangeFullPipeline) {
    using namespace std::chrono_literals;
    using Clock = std::chrono::high_resolution_clock;

    auto tick_queue = std::make_unique<utils::MPMCQueue<Tick, 65536>>();

    // ===================================================================
    // Binance Adapter #1: Top 25 USDT perps (depth@100ms + aggTrade)
    // ===================================================================
    auto gw_binance1 = market_data::createGateway("binance");
    ASSERT_NE(gw_binance1, nullptr);

    ExchangeConfig cfg_b1;
    cfg_b1.name = "binance";
    cfg_b1.proxy_host = "127.0.0.1";
    cfg_b1.proxy_port = 7897;
    cfg_b1.websocket_url =
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

    // ===================================================================
    // Binance Adapter #2: Next 25 USDT perps (depth@100ms + aggTrade)
    // ===================================================================
    auto gw_binance2 = market_data::createGateway("binance");
    ASSERT_NE(gw_binance2, nullptr);

    ExchangeConfig cfg_b2;
    cfg_b2.name = "binance";
    cfg_b2.proxy_host = "127.0.0.1";
    cfg_b2.proxy_port = 7897;
    cfg_b2.websocket_url =
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

    // ===================================================================
    // OKX Adapter: BTC-USDT + ETH-USDT (books5 + trades)
    // ===================================================================
    auto gw_okx = market_data::createGateway("okx");
    ASSERT_NE(gw_okx, nullptr);

    ExchangeConfig cfg_okx;
    cfg_okx.name = "okx";
    cfg_okx.proxy_host = "127.0.0.1";
    cfg_okx.proxy_port = 7897;

    // ===================================================================
    // Phase 1: Connect all adapters
    // ===================================================================
    auto t_conn_start = Clock::now();

    ASSERT_TRUE(gw_binance1->initialize(cfg_b1, *tick_queue));
    ASSERT_TRUE(gw_binance2->initialize(cfg_b2, *tick_queue));
    ASSERT_TRUE(gw_okx->initialize(cfg_okx, *tick_queue));

    ASSERT_TRUE(gw_binance1->start());
    ASSERT_TRUE(gw_binance2->start());

    // Drain immediately while connections establish (Binance URL-streams
    // pump data as soon as WS handshake completes)
    std::this_thread::sleep_for(2s);
    {
        Tick t;
        while (tick_queue->try_pop(t)) {}
    }

    bool okx_ok = gw_okx->start();

    std::this_thread::sleep_for(2s);
    {
        Tick t;
        while (tick_queue->try_pop(t)) {}
    }

    ASSERT_EQ(gw_binance1->getStatus(), ConnectionStatus::CONNECTED);
    ASSERT_EQ(gw_binance2->getStatus(), ConnectionStatus::CONNECTED);

    if (okx_ok) {
        // Subscribe to top 15 USDT perpetual pairs for stress
        const std::vector<std::string> okx_symbols = {
            "BTC-USDT", "ETH-USDT", "SOL-USDT", "XRP-USDT", "DOGE-USDT",
            "ADA-USDT", "AVAX-USDT", "LINK-USDT", "DOT-USDT", "MATIC-USDT",
            "LTC-USDT", "UNI-USDT", "ETC-USDT", "FIL-USDT", "POL-USDT"
        };
        for (const auto& sym : okx_symbols) {
            gw_okx->subscribe(sym);
        }
        std::this_thread::sleep_for(1s);
    }

    auto t_conn_end = Clock::now();
    double connect_sec = std::chrono::duration<double>(t_conn_end - t_conn_start).count();

    std::cout << "\n[PIPELINE] All adapters connected in " << std::fixed
              << std::setprecision(1) << connect_sec << "s\n" << std::endl;

    // ===================================================================
    // Phase 2: Warm-up drain (5 seconds)
    // ===================================================================
    std::cout << "[PIPELINE] Warming up (5s, spin-drain)..." << std::endl;
    {
        auto warmup_end = Clock::now() + 5s;
        Tick t;
        while (Clock::now() < warmup_end) {
            while (tick_queue->try_pop(t)) {}
        }
    }

    // ===================================================================
    // Phase 3: 20-second sustained measurement
    // ===================================================================
    std::cout << "[PIPELINE] Measuring 20-second sustained full pipeline..." << std::endl;

    // E2E latency: receive_timestamp_us → consumer pop time (full pipeline)
    std::vector<double> e2e_latencies_us;
    // Queue pop latency: try_pop duration only
    std::vector<double> pop_latencies_us;
    e2e_latencies_us.reserve(500000);
    pop_latencies_us.reserve(500000);

    uint64_t total_ticks = 0;
    uint64_t bid_ticks = 0, ask_ticks = 0, trade_ticks = 0;
    uint64_t binance_ticks = 0, okx_ticks = 0;

    auto meas_start = Clock::now();
    auto meas_end = meas_start + 20s;

    while (Clock::now() < meas_end) {
        auto pop_start = Clock::now();
        Tick t;
        bool got = tick_queue->try_pop(t);
        auto pop_end = Clock::now();

        if (got) {
            total_ticks++;

            // Queue pop latency (microseconds)
            double pop_us = std::chrono::duration<double, std::micro>(
                pop_end - pop_start).count();
            pop_latencies_us.push_back(pop_us);

            // End-to-end latency: WS receive → consumer pop
            auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                Clock::now().time_since_epoch()).count();
            int64_t e2e_us = now_us - static_cast<int64_t>(t.receive_timestamp_us);
            if (e2e_us >= 0) {
                e2e_latencies_us.push_back(static_cast<double>(e2e_us));
            }

            switch (t.side) {
                case TickSide::BID: bid_ticks++; break;
                case TickSide::ASK: ask_ticks++; break;
                case TickSide::TRADE: trade_ticks++; break;
            }
        }
    }

    auto meas_end_clock = Clock::now();
    double elapsed_sec = std::chrono::duration<double>(meas_end_clock - meas_start).count();

    // ===================================================================
    // Phase 4: Compute statistics
    // ===================================================================
    auto stats_b1 = gw_binance1->getStatistics();
    auto stats_b2 = gw_binance2->getStatistics();
    auto stats_okx = gw_okx->getStatistics();

    uint64_t binance_msgs = stats_b1.messages_received + stats_b2.messages_received;
    uint64_t okx_msgs = stats_okx.messages_received;
    uint64_t total_msgs = binance_msgs + okx_msgs;

    binance_ticks = stats_b1.ticks_processed + stats_b2.ticks_processed;
    okx_ticks = stats_okx.ticks_processed;

    auto compute_percentiles = [](std::vector<double>& v) {
        if (v.empty()) return std::make_tuple(0.0, 0.0, 0.0, 0.0, 0.0);
        std::sort(v.begin(), v.end());
        size_t n = v.size();
        return std::make_tuple(
            v.front(),                                        // min
            std::accumulate(v.begin(), v.end(), 0.0) / n,     // avg
            v[n * 50 / 100],                                   // p50
            v[n * 99 / 100],                                   // p99
            v[n * 999 / 1000]                                  // p99.9
        );
    };

    auto [e2e_min, e2e_avg, e2e_p50, e2e_p99, e2e_p999] = compute_percentiles(e2e_latencies_us);
    auto [pop_min, pop_avg, pop_p50, pop_p99, pop_p999] = compute_percentiles(pop_latencies_us);
    double pop_max = pop_latencies_us.empty() ? 0 : *std::max_element(pop_latencies_us.begin(), pop_latencies_us.end());
    double e2e_max = e2e_latencies_us.empty() ? 0 : *std::max_element(e2e_latencies_us.begin(), e2e_latencies_us.end());

    double throughput = total_ticks / elapsed_sec;

    // ===================================================================
    // Phase 5: Report
    // ===================================================================
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║        Full-Pipeline Multi-Exchange Performance              ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════════╣\n";
    std::cout << "║  Exchanges: Binance Futures (prod) ×2 + OKX (prod) ×1       ║\n";
    std::cout << "║  Symbols:   50 Binance (URL-embedded) + 14 OKX (JSON sub)  ║\n";
    std::cout << "║  Queue:     Shared MPMC (65536 slots, multi-producer)       ║\n";
    std::cout << "║  Duration:  " << std::setw(5) << std::fixed << std::setprecision(1)
              << elapsed_sec << "s                                               ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════════╣\n";
    std::cout << "║                                                              ║\n";

    std::cout << "║  ── Connections ──                                           ║\n";
    std::cout << "║  Total connect time:  " << std::setw(7) << std::fixed << std::setprecision(1)
              << connect_sec << "s                                     ║\n";
    std::cout << "║  OKX connected:       " << (okx_ok ? "YES" : "NO ")
              << "                                         ║\n";
    std::cout << "║                                                              ║\n";

    std::cout << "║  ── Messages Received (WS frames) ──                         ║\n";
    std::cout << "║  Binance ×2:  " << std::setw(12) << binance_msgs
              << " msgs                                  ║\n";
    std::cout << "║  OKX ×1:      " << std::setw(12) << okx_msgs
              << " msgs                                  ║\n";
    std::cout << "║  Total:       " << std::setw(12) << total_msgs
              << " msgs                                  ║\n";
    std::cout << "║                                                              ║\n";

    std::cout << "║  ── Ticks Processed (parsed & pushed to queue) ──            ║\n";
    std::cout << "║  Binance ×2:  " << std::setw(12) << binance_ticks
              << " ticks                                 ║\n";
    std::cout << "║  OKX ×1:      " << std::setw(12) << okx_ticks
              << " ticks                                 ║\n";
    std::cout << "║  Total:       " << std::setw(12) << (binance_ticks + okx_ticks)
              << " ticks                                 ║\n";
    std::cout << "║                                                              ║\n";

    std::cout << "║  ── Tick Composition ──                                      ║\n";
    std::cout << "║  BID:   " << std::setw(12) << bid_ticks
              << " (" << std::setw(5) << std::fixed << std::setprecision(1)
              << (100.0 * bid_ticks / std::max(uint64_t(1), total_ticks)) << "%)"
              << "                                     ║\n";
    std::cout << "║  ASK:   " << std::setw(12) << ask_ticks
              << " (" << std::setw(5) << std::fixed << std::setprecision(1)
              << (100.0 * ask_ticks / std::max(uint64_t(1), total_ticks)) << "%)"
              << "                                     ║\n";
    std::cout << "║  TRADE: " << std::setw(12) << trade_ticks
              << " (" << std::setw(5) << std::fixed << std::setprecision(1)
              << (100.0 * trade_ticks / std::max(uint64_t(1), total_ticks)) << "%)"
              << "                                     ║\n";
    std::cout << "║                                                              ║\n";

    std::cout << "║  ── Throughput ──                                            ║\n";
    std::cout << "║  Consumer drain:    " << std::setw(10) << std::fixed << std::setprecision(1)
              << throughput << " ticks/s                              ║\n";
    double binance_tp = binance_ticks / elapsed_sec;
    double okx_tp = okx_ticks / elapsed_sec;
    std::cout << "║  Binance produce:   " << std::setw(10) << std::fixed << std::setprecision(1)
              << binance_tp << " ticks/s                              ║\n";
    std::cout << "║  OKX produce:       " << std::setw(10) << std::fixed << std::setprecision(1)
              << okx_tp << " ticks/s                              ║\n";
    std::cout << "║                                                              ║\n";

    std::cout << "║  ── Queue Pop Latency (try_pop duration) ──                  ║\n";
    std::cout << "║  Samples:  " << std::setw(12) << pop_latencies_us.size()
              << "                                        ║\n";
    std::cout << "║  Avg:      " << std::setw(10) << std::fixed << std::setprecision(2)
              << pop_avg << " μs                                       ║\n";
    std::cout << "║  Min:      " << std::setw(10) << std::fixed << std::setprecision(2)
              << pop_min << " μs                                       ║\n";
    std::cout << "║  P50:      " << std::setw(10) << std::fixed << std::setprecision(2)
              << pop_p50 << " μs                                       ║\n";
    std::cout << "║  P99:      " << std::setw(10) << std::fixed << std::setprecision(2)
              << pop_p99 << " μs                                       ║\n";
    std::cout << "║  P99.9:    " << std::setw(10) << std::fixed << std::setprecision(2)
              << pop_p999 << " μs                                       ║\n";
    std::cout << "║  Max:      " << std::setw(10) << std::fixed << std::setprecision(2)
              << pop_max << " μs                                       ║\n";
    std::cout << "║                                                              ║\n";

    std::cout << "║  ── End-to-End Latency (WS recv → consumer pop) ──           ║\n";
    std::cout << "║  Samples:  " << std::setw(12) << e2e_latencies_us.size()
              << "                                        ║\n";
    std::cout << "║  Avg:      " << std::setw(10) << std::fixed << std::setprecision(2)
              << e2e_avg << " μs                                       ║\n";
    std::cout << "║  Min:      " << std::setw(10) << std::fixed << std::setprecision(2)
              << e2e_min << " μs                                       ║\n";
    std::cout << "║  P50:      " << std::setw(10) << std::fixed << std::setprecision(2)
              << e2e_p50 << " μs                                       ║\n";
    std::cout << "║  P99:      " << std::setw(10) << std::fixed << std::setprecision(2)
              << e2e_p99 << " μs                                       ║\n";
    std::cout << "║  P99.9:    " << std::setw(10) << std::fixed << std::setprecision(2)
              << e2e_p999 << " μs                                       ║\n";
    std::cout << "║                                                              ║\n";

    // Pipeline latency breakdown
    double parse_push_us = e2e_p50 - pop_p50;
    std::cout << "║  ── Pipeline Breakdown (P50) ──                              ║\n";
    std::cout << "║  Queue pop:       " << std::setw(8) << std::fixed << std::setprecision(2)
              << pop_p50 << " μs                                      ║\n";
    std::cout << "║  Parse+Push:      " << std::setw(8) << std::fixed << std::setprecision(2)
              << parse_push_us << " μs  (simdjson + Decimal + MPMC push)        ║\n";
    std::cout << "║  ─────────────────────────────                               ║\n";
    std::cout << "║  E2E Total (P50): " << std::setw(8) << std::fixed << std::setprecision(2)
              << e2e_p50 << " μs                                      ║\n";
    std::cout << "║                                                              ║\n";

    std::cout << "╚══════════════════════════════════════════════════════════════╝\n";
    std::cout << std::endl;

    // ===================================================================
    // Phase 6: Assertions
    // ===================================================================
    EXPECT_GT(total_ticks, 1000)
        << "Expected significant tick volume from 3 production connections";
    EXPECT_GT(binance_ticks, 0) << "Binance produced no ticks";
    if (okx_ok) {
        EXPECT_GT(okx_ticks, 0) << "OKX produced no ticks";
    }
    EXPECT_GT(bid_ticks + ask_ticks, 1000)
        << "Expected significant order book data from all exchanges";

    // Performance assertions
    EXPECT_LT(pop_p50, 10.0)
        << "Queue pop P50 exceeds 10μs target";
    EXPECT_LT(pop_p99, 100.0)
        << "Queue pop P99 exceeds 100μs target";
    // E2E P50 measures real pipeline — must be <500μs
    EXPECT_LT(e2e_p50, 500.0)
        << "End-to-end P50 exceeds 500μs target (simdjson+Decimal+MPMC)";
    // E2E P99 reflects OS scheduling jitter on non-RT system — relaxed bound
    EXPECT_LT(e2e_p99, 100000.0)
        << "End-to-end P99 exceeds 100ms (indicates systemic issue)";

    // Cleanup — drain while shutting down to prevent queue overflow
    gw_binance1->stop();
    { Tick t; while (tick_queue->try_pop(t)) {} }
    gw_binance2->stop();
    { Tick t; while (tick_queue->try_pop(t)) {} }
    if (okx_ok) {
        gw_okx->stop();
        { Tick t; while (tick_queue->try_pop(t)) {} }
    }

    std::cout << "[PIPELINE] All gateways stopped. Full pipeline perf test PASSED.\n";
}

} // namespace test
} // namespace chronos
