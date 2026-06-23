#include <gtest/gtest.h>
#include <chronos/market_data/gateway.hpp>
#include <chronos/market_data/any_gateway.hpp>
#include <chronos/market_data/adapters/thin_mux_adapter.hpp>
#include <chronos/market_data/gateway_v2_factory.hpp>
#include <chronos/io/transports/transport.hpp>
#include <chronos/io/protocols/protocol.hpp>
#include <chronos/io/protocols/binance_json_protocol.hpp>
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
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>

namespace chronos {
namespace test {

// ============================================================================
// Mock Transport (for benchmark — zero network, instant response)
// ============================================================================
class BenchmarkTransport final : public market_data::Transport {
public:
    bool connect(const ExchangeConfig&) override {
        connected_ = true;
        return true;
    }
    void disconnect() override { connected_ = false; }
    bool send(const std::string&) override { return true; }
    bool sendPing() override { return true; }
    std::string receive() override {
        if (!connected_) return "";
        msg_counter_++;
        // Valid Binance futures depthUpdate format
        return R"({"e":"depthUpdate","E":1718000000000,"s":"BTCUSDT","b":[["50000.00","1.0"]],"a":[["50001.00","0.5"]]})";
    }
    bool isConnected() const override { return connected_; }
    void stop() override { connected_ = false; }

    uint64_t msg_counter() const { return msg_counter_; }
private:
    std::atomic<bool> connected_{false};
    std::atomic<uint64_t> msg_counter_{0};
};

// ============================================================================
// Micro-benchmark: New Architecture adapter overhead
// ============================================================================
class ArchitectureBenchmarkTest : public ::testing::Test {
protected:
    void SetUp() override {
        queue_ = std::make_unique<utils::MPMCQueue<Tick, 65536>>();
    }
    std::unique_ptr<utils::MPMCQueue<Tick, 65536>> queue_;
};

TEST_F(ArchitectureBenchmarkTest, NewArchitecture_SubscribePerformance) {
    using Clock = std::chrono::high_resolution_clock;

    // Measure subscribe() hot path
    auto transport = std::make_unique<BenchmarkTransport>();
    auto protocol  = std::make_unique<market_data::BinanceJsonProtocol>();

    market_data::ThinMuxAdapter adapter(std::move(transport), std::move(protocol));
    ExchangeConfig cfg;
    cfg.name = "bench";
    adapter.initialize(cfg, *queue_);

    const int ITERS = 1000;
    auto start = Clock::now();
    for (int i = 0; i < ITERS; ++i) {
        adapter.subscribe("btcusdt");
    }
    auto end = Clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    std::cout << "\n[NEW-ARCH] Subscribe: " << ITERS << " calls, "
              << ns / ITERS << " ns/call (dedup: 999 hits)\n";
    // Sanitizers add 3-50x overhead depending on the tool and compiler.
    // Clang:   __has_feature detects ASan/TSan/UBSan reliably.
    // GCC:     __SANITIZE_ADDRESS__ works for ASan, but TSan has no GCC macro.
    //          Fall back to a 50× relaxed threshold when no sanitizer is detected
    //          (covers GCC TSan where 3108 ns/call was observed on CI).
#if defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(undefined_behavior_sanitizer)
    constexpr int64_t threshold_ns = 50000;
#  else
    constexpr int64_t threshold_ns = 5000;
#  endif
#elif defined(__SANITIZE_ADDRESS__)
    constexpr int64_t threshold_ns = 50000;
#else
    // Includes GCC TSan (no dedicated macro) — relax 50× to avoid false positives.
    constexpr int64_t threshold_ns = 5000;
#endif
    EXPECT_LT(ns / ITERS, threshold_ns) << "Subscribe dedup should be <" << threshold_ns << "ns";
}

TEST_F(ArchitectureBenchmarkTest, NewArchitecture_TickPushThroughput) {
    using Clock = std::chrono::high_resolution_clock;

    // Pre-register symbols
    auto transport = std::make_unique<BenchmarkTransport>();
    auto* raw_transport = transport.get();
    auto protocol  = std::make_unique<market_data::BinanceJsonProtocol>();

    market_data::ThinMuxAdapter adapter(std::move(transport), std::move(protocol));
    ExchangeConfig cfg;
    cfg.name = "bench";
    cfg.heartbeat_interval_ms = 86400000; // 24h — never fire during test
    adapter.initialize(cfg, *queue_);
    adapter.subscribe("btcusdt");

    // Start adapter — readLoop will consume from BenchmarkTransport.receive()
    adapter.start();

    // Let it run for 2 seconds, count ticks
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // Drain queue
    uint64_t tick_count = 0;
    Tick t;
    while (queue_->try_pop(t)) tick_count++;

    adapter.stop();

    double ticks_per_sec = tick_count / 2.0;
    uint64_t msgs = raw_transport->msg_counter();
    double msgs_per_sec = msgs / 2.0;

    std::cout << "\n[NEW-ARCH] Tick throughput: " << std::fixed << std::setprecision(0)
              << ticks_per_sec << " ticks/s, "
              << msgs_per_sec << " msgs/s (simdjson parse + Decimal + MPMC push)\n";

    EXPECT_GT(tick_count, 0) << "No ticks pushed";
    // With no real I/O, throughput should be very high (>100k ticks/s)
    EXPECT_GT(ticks_per_sec, 10000) << "Throughput below expectation for mock transport";
}

TEST_F(ArchitectureBenchmarkTest, NewArchitecture_StartStopLatency) {
    using Clock = std::chrono::high_resolution_clock;

    const int ITERS = 100;
    std::vector<double> start_us, stop_us;

    for (int i = 0; i < ITERS; ++i) {
        auto transport = std::make_unique<BenchmarkTransport>();
        auto protocol  = std::make_unique<market_data::BinanceJsonProtocol>();

        market_data::ThinMuxAdapter adapter(std::move(transport), std::move(protocol));
        ExchangeConfig cfg;
        cfg.name = "bench";
        cfg.heartbeat_interval_ms = 86400000; // 24h — never fire during test
        adapter.initialize(cfg, *queue_);
        adapter.subscribe("btcusdt");

        auto t0 = Clock::now();
        adapter.start();
        auto t1 = Clock::now();
        start_us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());

        auto t2 = Clock::now();
        adapter.stop();
        auto t3 = Clock::now();
        stop_us.push_back(std::chrono::duration<double, std::micro>(t3 - t2).count());
    }

    auto pct = [](std::vector<double>& v, int p) {
        std::sort(v.begin(), v.end());
        return v[v.size() * p / 100];
    };

    std::cout << "\n[NEW-ARCH] Start/Stop (n=" << ITERS << "):\n"
              << "  start: min=" << pct(start_us, 0) << "μs P50=" << pct(start_us, 50)
              << "μs P99=" << pct(start_us, 99) << "μs\n"
              << "  stop:  min=" << pct(stop_us, 0) << "μs P50=" << pct(stop_us, 50)
              << "μs P99=" << pct(stop_us, 99) << "μs\n";

    EXPECT_LT(pct(start_us, 50), 5000) << "Start P50 > 5ms";
    EXPECT_LT(pct(stop_us, 50), 5000) << "Stop P50 > 5ms";
}

// ============================================================================
// Live Connectivity Comparison: Old vs New Architecture
// ============================================================================
//
// Connects both architectures to the SAME Binance testnet endpoint
// sequentially, measuring tick throughput and E2E latency.
//
// SKIPPED by default. Run with:
//   CHRONOS_LIVE_TEST=1 ./unit_tests --gtest_filter=ArchCompareLive*

class ArchCompareLiveTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!std::getenv("CHRONOS_LIVE_TEST")) {
            GTEST_SKIP() << "Set CHRONOS_LIVE_TEST=1";
        }
    }
};

struct ComparisonMetrics {
    std::string label;
    size_t total_ticks = 0;
    size_t bid_ticks = 0, ask_ticks = 0, trade_ticks = 0;
    uint64_t messages_received = 0;
    uint64_t ticks_processed = 0;
    uint64_t errors = 0;
    double elapsed_sec = 0;
    double throughput = 0;
    double pop_p50_us = 0;
    double pop_p99_us = 0;
    double pop_avg_us = 0;
    double pop_max_us = 0;
    double connect_sec = 0;

    static ComparisonMetrics measure(
        std::function<std::unique_ptr<market_data::MarketDataGateway>()> old_factory,
        std::function<market_data::AnyGateway()> new_factory,
        const std::string& label,
        utils::MPMCQueue<Tick, 65536>& queue,
        int duration_sec)
    {
        using Clock = std::chrono::high_resolution_clock;
        using namespace std::chrono_literals;

        ComparisonMetrics m;
        m.label = label;

        // Drain queue first
        { Tick t; while (queue.try_pop(t)) {} }

        std::vector<double> pop_latencies;
        pop_latencies.reserve(500000);

        auto meas_start = Clock::now();
        auto meas_end = meas_start + std::chrono::seconds(duration_sec);

        while (Clock::now() < meas_end) {
            auto pop_start = Clock::now();
            Tick t;
            bool got = queue.try_pop(t);
            auto pop_end = Clock::now();

            if (got) {
                m.total_ticks++;
                double pop_us = std::chrono::duration<double, std::micro>(
                    pop_end - pop_start).count();
                pop_latencies.push_back(pop_us);

                switch (t.side) {
                    case TickSide::BID:  m.bid_ticks++; break;
                    case TickSide::ASK:  m.ask_ticks++; break;
                    case TickSide::TRADE: m.trade_ticks++; break;
                }
            }
        }

        auto meas_end_clock = Clock::now();
        m.elapsed_sec = std::chrono::duration<double>(meas_end_clock - meas_start).count();
        m.throughput = m.total_ticks / m.elapsed_sec;

        // Compute pop latency percentiles
        if (!pop_latencies.empty()) {
            std::sort(pop_latencies.begin(), pop_latencies.end());
            size_t n = pop_latencies.size();
            m.pop_avg_us = std::accumulate(pop_latencies.begin(), pop_latencies.end(), 0.0) / n;
            m.pop_p50_us = pop_latencies[n * 50 / 100];
            m.pop_p99_us = pop_latencies[n * 99 / 100];
            m.pop_max_us = pop_latencies.back();
        }

        return m;
    }
};

TEST_F(ArchCompareLiveTest, BinanceSingleStream_OldVsNew) {
    using namespace std::chrono_literals;
    using Clock = std::chrono::high_resolution_clock;

    const int DURATION_SEC = 5;

    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║  Architecture Comparison: Old vs New                         ║\n";
    std::cout << "║  Exchange: Binance Futures Testnet (single stream)           ║\n";
    std::cout << "║  Duration: " << DURATION_SEC << "s per architecture                       ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n";
    std::cout << std::endl;

    // =========================================================================
    // OLD Architecture
    // =========================================================================
    std::cout << "[OLD-ARCH] Testing MarketDataGateway + BinanceAdapter...\n";

    auto old_queue = std::make_unique<utils::MPMCQueue<Tick, 65536>>();
    auto old_gw = market_data::createGateway("binance");
    ASSERT_NE(old_gw, nullptr);

    ExchangeConfig old_cfg;
    old_cfg.name = "binance";
    old_cfg.websocket_url = "wss://stream.binancefuture.com/ws/btcusdt@depth@100ms";
    old_cfg.symbols = {"btcusdt"};

    auto t0 = Clock::now();
    ASSERT_TRUE(old_gw->initialize(old_cfg, *old_queue));
    ASSERT_TRUE(old_gw->start());
    std::this_thread::sleep_for(2s);
    auto t1 = Clock::now();
    double old_connect_sec = std::chrono::duration<double>(t1 - t0).count();

    ASSERT_EQ(old_gw->getStatus(), ConnectionStatus::CONNECTED);
    ASSERT_TRUE(old_gw->subscribe("btcusdt"));

    std::cout << "[OLD-ARCH] Connected in " << old_connect_sec << "s, draining...\n";

    auto old_metrics = ComparisonMetrics::measure(
        nullptr, nullptr, "OLD",
        *old_queue, DURATION_SEC);

    auto old_stats = old_gw->getStatistics();
    old_metrics.messages_received = old_stats.messages_received;
    old_metrics.ticks_processed = old_stats.ticks_processed;
    old_metrics.errors = old_stats.errors_count;
    old_metrics.connect_sec = old_connect_sec;

    old_gw->stop();
    std::cout << "[OLD-ARCH] Stopped.\n\n";

    // =========================================================================
    // NEW Architecture
    // =========================================================================
    std::cout << "[NEW-ARCH] Testing AnyGateway + ThinMuxAdapter...\n";

    auto new_queue = std::make_unique<utils::MPMCQueue<Tick, 65536>>();
    ExchangeConfig new_cfg;
    new_cfg.name = "binance";
    new_cfg.websocket_url = "wss://stream.binancefuture.com/ws/btcusdt@depth@100ms";
    new_cfg.symbols = {"btcusdt"};

    t0 = Clock::now();
    auto new_gw = market_data::createGatewayV2("binance", new_cfg, *new_queue);
    ASSERT_TRUE(static_cast<bool>(new_gw));
    ASSERT_TRUE(new_gw.subscribe("btcusdt"));
    ASSERT_TRUE(new_gw.start());
    std::this_thread::sleep_for(2s);
    t1 = Clock::now();
    double new_connect_sec = std::chrono::duration<double>(t1 - t0).count();

    ASSERT_EQ(new_gw.getStatus(), ConnectionStatus::CONNECTED);

    std::cout << "[NEW-ARCH] Connected in " << new_connect_sec << "s, draining...\n";

    auto new_metrics = ComparisonMetrics::measure(
        nullptr, nullptr, "NEW",
        *new_queue, DURATION_SEC);

    auto new_stats = new_gw.getStatistics();
    new_metrics.messages_received = new_stats.messages_received;
    new_metrics.ticks_processed = new_stats.ticks_processed;
    new_metrics.errors = new_stats.errors_count;
    new_metrics.connect_sec = new_connect_sec;

    new_gw.stop();
    std::cout << "[NEW-ARCH] Stopped.\n\n";

    // =========================================================================
    // Print Comparison Table
    // =========================================================================
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║               OLD vs NEW Architecture — Side-by-Side                     ║\n";
    std::cout << "╠══════════════════════════════════════════════════╦═══════════╦═══════════╣\n";
    std::cout << "║ Metric                                           ║ OLD       ║ NEW       ║\n";
    std::cout << "╠══════════════════════════════════════════════════╬═══════════╬═══════════╣\n";

    auto row_int = [](const char* name, size_t old_v, size_t new_v, const char* unit) {
        std::ostringstream oss;
        oss << "║ " << std::left << std::setw(48) << name
            << " ║ " << std::right << std::setw(7) << old_v << " " << std::left << std::setw(3) << unit
            << "║ " << std::right << std::setw(7) << new_v << " " << std::left << std::setw(3) << unit
            << "║\n";
        return oss.str();
    };
    auto row_f = [](const char* name, double old_v, double new_v, const char* unit) {
        std::ostringstream oss;
        oss << "║ " << std::left << std::setw(48) << name
            << " ║ " << std::right << std::setw(7) << std::fixed << std::setprecision(1) << old_v << " " << std::left << std::setw(3) << unit
            << "║ " << std::right << std::setw(7) << std::fixed << std::setprecision(1) << new_v << " " << std::left << std::setw(3) << unit
            << "║\n";
        return oss.str();
    };
    auto row_pct = [](const char* name, double old_v, double new_v, const char* unit) {
        std::ostringstream oss;
        oss << "║ " << std::left << std::setw(48) << name
            << " ║ " << std::right << std::setw(7) << std::fixed << std::setprecision(2) << old_v << " " << std::left << std::setw(3) << unit
            << "║ " << std::right << std::setw(7) << std::fixed << std::setprecision(2) << new_v << " " << std::left << std::setw(3) << unit;
        if (old_v > 0 && new_v > 0) {
            double diff = ((new_v - old_v) / old_v) * 100.0;
            oss << " [" << std::showpos << std::fixed << std::setprecision(1) << diff << "%]";
        }
        oss << " ║\n";
        return oss.str();
    };

    std::cout << row_f("Connect time (s)", old_metrics.connect_sec, new_metrics.connect_sec, "s");
    std::cout << "╠══════════════════════════════════════════════════╬═══════════╬═══════════╣\n";
    std::cout << row_int("Total ticks drained", old_metrics.total_ticks, new_metrics.total_ticks, "");
    std::cout << row_int("  BID ticks", old_metrics.bid_ticks, new_metrics.bid_ticks, "");
    std::cout << row_int("  ASK ticks", old_metrics.ask_ticks, new_metrics.ask_ticks, "");
    std::cout << row_int("  TRADE ticks", old_metrics.trade_ticks, new_metrics.trade_ticks, "");
    std::cout << row_int("Messages received (WS)", old_metrics.messages_received, new_metrics.messages_received, "");
    std::cout << row_int("Ticks processed (pushed)", old_metrics.ticks_processed, new_metrics.ticks_processed, "");
    std::cout << "╠══════════════════════════════════════════════════╬═══════════╬═══════════╣\n";
    std::cout << row_pct("Throughput (ticks/s)", old_metrics.throughput, new_metrics.throughput, "");
    std::cout << "╠══════════════════════════════════════════════════╬═══════════╬═══════════╣\n";
    std::cout << row_pct("Queue Pop P50 (μs)", old_metrics.pop_p50_us, new_metrics.pop_p50_us, "μs");
    std::cout << row_pct("Queue Pop P99 (μs)", old_metrics.pop_p99_us, new_metrics.pop_p99_us, "μs");
    std::cout << row_pct("Queue Pop Avg (μs)", old_metrics.pop_avg_us, new_metrics.pop_avg_us, "μs");
    std::cout << row_pct("Queue Pop Max (μs)", old_metrics.pop_max_us, new_metrics.pop_max_us, "μs");
    std::cout << "╠══════════════════════════════════════════════════╬═══════════╬═══════════╣\n";
    std::cout << row_int("Errors", old_metrics.errors, new_metrics.errors, "");
    std::cout << "╚══════════════════════════════════════════════════╩═══════════╩═══════════╝\n";
    std::cout << std::endl;

    // Assertions
    EXPECT_GT(old_metrics.total_ticks, 0) << "OLD: No ticks received";
    EXPECT_GT(new_metrics.total_ticks, 0) << "NEW: No ticks received";

    // Both should achieve similar throughput (±30%)
    double ratio = new_metrics.throughput / std::max(1.0, old_metrics.throughput);
    EXPECT_GT(ratio, 0.7) << "NEW throughput < 70% of OLD — regression";
    EXPECT_LT(ratio, 1.3) << "NEW throughput > 130% of OLD — anomalous";

    // Queue pop latency should be comparable (same queue implementation)
    EXPECT_LT(new_metrics.pop_p50_us, 20.0) << "NEW pop P50 > 20μs — regression";
}

TEST_F(ArchCompareLiveTest, BinanceMultiSymbol_OldVsNew) {
    using namespace std::chrono_literals;
    using Clock = std::chrono::high_resolution_clock;

    const int DURATION_SEC = 8;

    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║  Architecture Comparison: Old vs New (Multi-Symbol)          ║\n";
    std::cout << "║  Exchange: Binance Futures Production (25 symbols ×2)        ║\n";
    std::cout << "║  Duration: " << DURATION_SEC << "s per architecture                       ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n";
    std::cout << std::endl;

    const std::string URL_25_SYMBOLS =
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

    // =========================================================================
    // OLD Architecture
    // =========================================================================
    std::cout << "[OLD-ARCH] Testing MarketDataGateway + BinanceAdapter (25 symbols)...\n";

    auto old_queue = std::make_unique<utils::MPMCQueue<Tick, 65536>>();
    auto old_gw = market_data::createGateway("binance");
    ASSERT_NE(old_gw, nullptr);

    ExchangeConfig old_cfg;
    old_cfg.name = "binance";
    old_cfg.proxy_host = "127.0.0.1";
    old_cfg.proxy_port = 7897;
    old_cfg.websocket_url = URL_25_SYMBOLS;

    ASSERT_TRUE(old_gw->initialize(old_cfg, *old_queue));

    auto t0 = Clock::now();
    ASSERT_TRUE(old_gw->start());
    std::this_thread::sleep_for(3s);
    auto t1 = Clock::now();
    double old_connect_sec = std::chrono::duration<double>(t1 - t0).count();

    ASSERT_EQ(old_gw->getStatus(), ConnectionStatus::CONNECTED);
    std::cout << "[OLD-ARCH] Connected in " << old_connect_sec << "s, draining...\n";

    auto old_metrics = ComparisonMetrics::measure(nullptr, nullptr, "OLD", *old_queue, DURATION_SEC);

    auto old_stats = old_gw->getStatistics();
    old_metrics.messages_received = old_stats.messages_received;
    old_metrics.ticks_processed = old_stats.ticks_processed;
    old_metrics.errors = old_stats.errors_count;
    old_metrics.connect_sec = old_connect_sec;

    old_gw->stop();
    { Tick t; while (old_queue->try_pop(t)) {} }
    std::cout << "[OLD-ARCH] Stopped.\n\n";

    // =========================================================================
    // NEW Architecture
    // =========================================================================
    std::cout << "[NEW-ARCH] Testing AnyGateway + ThinMuxAdapter (25 symbols)...\n";

    // Extract symbols from URL streams (same as old BinanceAdapter does internally)
    std::vector<std::string> symbols_25 = {
        "btcusdt","ethusdt","solusdt","bnbusdt","xrpusdt",
        "dogeusdt","adausdt","avaxusdt","linkusdt","dotusdt",
        "maticusdt","ltcusdt","uniusdt","etcusdt","filusdt",
        "atomusdt","aptusdt","arbusdt","opusdt","suiusdt",
        "nearusdt","tiausdt","injusdt","stxusdt","seiusdt"
    };

    auto new_queue = std::make_unique<utils::MPMCQueue<Tick, 65536>>();
    ExchangeConfig new_cfg;
    new_cfg.name = "binance";
    new_cfg.proxy_host = "127.0.0.1";
    new_cfg.proxy_port = 7897;
    new_cfg.websocket_url = URL_25_SYMBOLS;

    t0 = Clock::now();
    auto new_gw = market_data::createGatewayV2("binance", new_cfg, *new_queue);
    ASSERT_TRUE(static_cast<bool>(new_gw));

    // Pre-register all symbols (new architecture requires explicit subscribe)
    for (const auto& sym : symbols_25) {
        new_gw.subscribe(sym);
    }

    ASSERT_TRUE(new_gw.start());
    std::this_thread::sleep_for(3s);
    t1 = Clock::now();
    double new_connect_sec = std::chrono::duration<double>(t1 - t0).count();

    ASSERT_EQ(new_gw.getStatus(), ConnectionStatus::CONNECTED);
    std::cout << "[NEW-ARCH] Connected in " << new_connect_sec << "s, draining...\n";

    auto new_metrics = ComparisonMetrics::measure(nullptr, nullptr, "NEW", *new_queue, DURATION_SEC);

    auto new_stats = new_gw.getStatistics();
    new_metrics.messages_received = new_stats.messages_received;
    new_metrics.ticks_processed = new_stats.ticks_processed;
    new_metrics.errors = new_stats.errors_count;
    new_metrics.connect_sec = new_connect_sec;

    new_gw.stop();
    { Tick t; while (new_queue->try_pop(t)) {} }
    std::cout << "[NEW-ARCH] Stopped.\n\n";

    // =========================================================================
    // Print Comparison
    // =========================================================================
    auto row_int = [](const char* name, size_t old_v, size_t new_v, const char* unit) {
        std::ostringstream oss;
        oss << "║ " << std::left << std::setw(48) << name
            << " ║ " << std::right << std::setw(9) << old_v << " " << std::left << std::setw(3) << unit
            << "║ " << std::right << std::setw(9) << new_v << " " << std::left << std::setw(3) << unit
            << "║\n";
        return oss.str();
    };

    auto row_f = [](const char* name, double old_v, double new_v, const char* unit) {
        std::ostringstream oss;
        oss << "║ " << std::left << std::setw(48) << name
            << " ║ " << std::right << std::setw(8) << std::fixed << std::setprecision(1) << old_v << " " << std::left << std::setw(3) << unit
            << "║ " << std::right << std::setw(8) << std::fixed << std::setprecision(1) << new_v << " " << std::left << std::setw(3) << unit;
        return oss.str();
    };

    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║       OLD vs NEW — 25-Symbol Production Comparison                               ║\n";
    std::cout << "╠══════════════════════════════════════════════════╦═════════════╦═════════════════╣\n";
    std::cout << "║ Metric                                           ║ OLD         ║ NEW             ║\n";
    std::cout << "╠══════════════════════════════════════════════════╬═════════════╬═════════════════╣\n";
    std::cout << row_f("Connect time (s)", old_metrics.connect_sec, new_metrics.connect_sec, "s");
    std::cout << row_int("Total ticks", old_metrics.total_ticks, new_metrics.total_ticks, "");
    std::cout << row_int("Messages (WS frames)", old_metrics.messages_received, new_metrics.messages_received, "");
    std::cout << row_int("Ticks pushed to queue", old_metrics.ticks_processed, new_metrics.ticks_processed, "");
    std::cout << "╠══════════════════════════════════════════════════╬═════════════╬═════════════════╣\n";
    std::cout << row_f("Throughput (ticks/s)", old_metrics.throughput, new_metrics.throughput, "");
    std::cout << row_f("Pop P50 (μs)", old_metrics.pop_p50_us, new_metrics.pop_p50_us, "μs");
    std::cout << row_f("Pop P99 (μs)", old_metrics.pop_p99_us, new_metrics.pop_p99_us, "μs");
    std::cout << row_f("Pop Avg (μs)", old_metrics.pop_avg_us, new_metrics.pop_avg_us, "μs");
    std::cout << "╠══════════════════════════════════════════════════╬═════════════╬═════════════════╣\n";
    std::cout << row_int("Errors", old_metrics.errors, new_metrics.errors, "");
    std::cout << "╚══════════════════════════════════════════════════╩═════════════╩═════════════════╝\n";
    std::cout << std::endl;

    // Assertions
    EXPECT_GT(old_metrics.total_ticks, 0);
    EXPECT_GT(new_metrics.total_ticks, 0);
    double ratio = new_metrics.throughput / std::max(1.0, old_metrics.throughput);
    EXPECT_GT(ratio, 0.7) << "NEW throughput significantly lower than OLD";
    EXPECT_LT(new_metrics.pop_p50_us, 20.0) << "NEW pop P50 > 20μs";
}

} // namespace test
} // namespace chronos
