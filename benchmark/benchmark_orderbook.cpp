/**
 * @file benchmark_orderbook.cpp
 * @brief Performance benchmarks for OrderBook V1 and V2
 * 
 * Latency targets:
 * - OrderBook V1: Hot path (best bid/ask) <10ns
 * - OrderBook V2: Hot path (top 5 levels) <10ns
 * - OrderBook V2: Cold path (full 20 levels) <50ns
 * - Update latency: <100ns
 */

#include <benchmark/benchmark.h>
#include <chronos/market_data/orderbook.hpp>
#include <chronos/market_data/orderbook_v2.hpp>
#include <chronos/core/types.hpp>
#include <random>
#include <vector>

using namespace chronos;
using namespace chronos::market_data;

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * @brief Create a tick with specified price and quantity
 * @param side BID / ASK / TRADE
 */
Tick createTick(double price, double quantity, TickSide side) {
    Tick tick;
    tick.symbol_id = 1;
    tick.exchange_id = 1;
    tick.exchange_timestamp_us = 1000000;
    tick.receive_timestamp_us = 1000001;
    tick.price = toDecimal(price);
    tick.quantity = toDecimal(quantity);
    tick.side = side;
    tick.flags = 0;
    return tick;
}

/**
 * @brief Populate orderbook with realistic market data
 */
template<typename OrderBookType>
void populateOrderBook(OrderBookType& book, int levels = 20) {
    // Base price around 50000 (like BTC)
    double base_price = 50000.0;
    double tick_size = 0.01;
    
    // Add bid levels (descending from base_price)
    for (int i = 0; i < levels; ++i) {
        double price = base_price - (i * tick_size);
        double quantity = 1.0 + (i * 0.1);
        book.update(createTick(price, quantity, TickSide::BID));
    }
    
    // Add ask levels (ascending from base_price + tick_size)
    for (int i = 0; i < levels; ++i) {
        double price = base_price + tick_size + (i * tick_size);
        double quantity = 1.0 + (i * 0.1);
        book.update(createTick(price, quantity, TickSide::ASK));
    }
}

/**
 * @brief Pre-generate a large batch of random ticks to defeat branch prediction 
 * and L1/L2 cache hits during benchmarking, without adding RNG overhead to the loop.
 */
std::vector<Tick> generateRandomTicks(size_t count, double base_price = 50000.0) {
    std::vector<Tick> ticks(count);
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> price_dist(base_price - 10.0, base_price + 10.0);
    std::uniform_real_distribution<> qty_dist(0.1, 5.0);
    std::uniform_int_distribution<int> side_dist(0, 1);
    
    for (size_t i = 0; i < count; ++i) {
        ticks[i] = createTick(price_dist(gen), qty_dist(gen), static_cast<TickSide>(side_dist(gen)));
    }
    return ticks;
}

// ============================================================================
// OrderBook V1 Benchmarks
// ============================================================================

/**
 * @brief Benchmark: OrderBook V1 - getBestBid (hot path)
 * Target: <10ns
 */
static void BM_OrderBookV1_GetBestBid(benchmark::State& state) {
    OrderBook book;
    populateOrderBook(book, 20);
    
    for (auto _ : state) {
        auto bid = book.getBestBid();
        benchmark::DoNotOptimize(bid);
    }
    
    state.SetLabel("V1 Hot Path");
}
BENCHMARK(BM_OrderBookV1_GetBestBid);

/**
 * @brief Benchmark: OrderBook V1 - getBestAsk (hot path)
 * Target: <10ns
 */
static void BM_OrderBookV1_GetBestAsk(benchmark::State& state) {
    OrderBook book;
    populateOrderBook(book, 20);
    
    for (auto _ : state) {
        auto ask = book.getBestAsk();
        benchmark::DoNotOptimize(ask);
    }
    
    state.SetLabel("V1 Hot Path");
}
BENCHMARK(BM_OrderBookV1_GetBestAsk);

/**
 * @brief Benchmark: OrderBook V1 - getMidPrice
 * Target: <20ns (requires both bid and ask)
 */
static void BM_OrderBookV1_GetMidPrice(benchmark::State& state) {
    OrderBook book;
    populateOrderBook(book, 20);
    
    for (auto _ : state) {
        auto mid = book.getMidPrice();
        benchmark::DoNotOptimize(mid);
    }
    
    state.SetLabel("V1 Mid Price");
}
BENCHMARK(BM_OrderBookV1_GetMidPrice);

/**
 * @brief Benchmark: OrderBook V1 - getSpread
 * Target: <20ns
 */
static void BM_OrderBookV1_GetSpread(benchmark::State& state) {
    OrderBook book;
    populateOrderBook(book, 20);
    
    for (auto _ : state) {
        auto spread = book.getSpread();
        benchmark::DoNotOptimize(spread);
    }
    
    state.SetLabel("V1 Spread");
}
BENCHMARK(BM_OrderBookV1_GetSpread);

/**
 * @brief Benchmark: OrderBook V1 - getBidLevels (cold path)
 * Target: <100ns for 20 levels
 */
static void BM_OrderBookV1_GetBidLevels(benchmark::State& state) {
    OrderBook book;
    populateOrderBook(book, 20);
    
    for (auto _ : state) {
        auto levels = book.getBidLevels(20);
        benchmark::DoNotOptimize(levels);
    }
    
    state.SetLabel("V1 Cold Path (20 levels)");
}
BENCHMARK(BM_OrderBookV1_GetBidLevels);

/**
 * @brief Benchmark: OrderBook V1 - getAskLevels (cold path)
 * Target: <100ns for 20 levels
 */
static void BM_OrderBookV1_GetAskLevels(benchmark::State& state) {
    OrderBook book;
    populateOrderBook(book, 20);
    
    for (auto _ : state) {
        auto levels = book.getAskLevels(20);
        benchmark::DoNotOptimize(levels);
    }
    
    state.SetLabel("V1 Cold Path (20 levels)");
}
BENCHMARK(BM_OrderBookV1_GetAskLevels);

/**
 * @brief Benchmark: OrderBook V1 - update (single level)
 * Target: <100ns
 */
static void BM_OrderBookV1_Update(benchmark::State& state) {
    OrderBook book;
    populateOrderBook(book, 20);
    
    // Pre-generate 131072 (2^17) random ticks (approx 6MB, exceeds L1/L2 cache)
    const size_t TICK_COUNT = 131072;
    const size_t MASK = TICK_COUNT - 1;
    auto ticks = generateRandomTicks(TICK_COUNT);
    size_t idx = 0;
    
    for (auto _ : state) {
        book.update(ticks[idx]);
        // Bitwise AND is much faster than modulo (%), zero overhead in the loop
        idx = (idx + 1) & MASK; 
        benchmark::DoNotOptimize(book);
    }
    
    state.SetLabel("V1 Update");
}
BENCHMARK(BM_OrderBookV1_Update);

// ============================================================================
// OrderBook V2 Benchmarks
// ============================================================================

/**
 * @brief Benchmark: OrderBook V2 - getBestBid (ultra-fast hot path)
 * Target: <10ns
 */
static void BM_OrderBookV2_GetBestBid(benchmark::State& state) {
    OrderBookV2 book;
    populateOrderBook(book, 20);
    
    for (auto _ : state) {
        auto bid = book.getBestBid();
        benchmark::DoNotOptimize(bid);
    }
    
    state.SetLabel("V2 Ultra-Fast Hot Path");
}
BENCHMARK(BM_OrderBookV2_GetBestBid);

/**
 * @brief Benchmark: OrderBook V2 - getBestAsk (ultra-fast hot path)
 * Target: <10ns
 */
static void BM_OrderBookV2_GetBestAsk(benchmark::State& state) {
    OrderBookV2 book;
    populateOrderBook(book, 20);
    
    for (auto _ : state) {
        auto ask = book.getBestAsk();
        benchmark::DoNotOptimize(ask);
    }
    
    state.SetLabel("V2 Ultra-Fast Hot Path");
}
BENCHMARK(BM_OrderBookV2_GetBestAsk);

/**
 * @brief Benchmark: OrderBook V2 - getMidPrice
 * Target: <10ns
 */
static void BM_OrderBookV2_GetMidPrice(benchmark::State& state) {
    OrderBookV2 book;
    populateOrderBook(book, 20);
    
    for (auto _ : state) {
        auto mid = book.getMidPrice();
        benchmark::DoNotOptimize(mid);
    }
    
    state.SetLabel("V2 Mid Price");
}
BENCHMARK(BM_OrderBookV2_GetMidPrice);

/**
 * @brief Benchmark: OrderBook V2 - getSpread
 * Target: <10ns
 */
static void BM_OrderBookV2_GetSpread(benchmark::State& state) {
    OrderBookV2 book;
    populateOrderBook(book, 20);
    
    for (auto _ : state) {
        auto spread = book.getSpread();
        benchmark::DoNotOptimize(spread);
    }
    
    state.SetLabel("V2 Spread");
}
BENCHMARK(BM_OrderBookV2_GetSpread);

/**
 * @brief Benchmark: OrderBook V2 - getTop5Bids (hot path)
 * Target: <10ns
 */
static void BM_OrderBookV2_GetTop5Bids(benchmark::State& state) {
    OrderBookV2 book;
    populateOrderBook(book, 20);
    
    for (auto _ : state) {
        auto levels = book.getTop5Bids();
        benchmark::DoNotOptimize(levels);
    }
    
    state.SetLabel("V2 Top 5 Bids (Hot Path)");
}
BENCHMARK(BM_OrderBookV2_GetTop5Bids);

/**
 * @brief Benchmark: OrderBook V2 - getTop5Asks (hot path)
 * Target: <10ns
 */
static void BM_OrderBookV2_GetTop5Asks(benchmark::State& state) {
    OrderBookV2 book;
    populateOrderBook(book, 20);
    
    for (auto _ : state) {
        auto levels = book.getTop5Asks();
        benchmark::DoNotOptimize(levels);
    }
    
    state.SetLabel("V2 Top 5 Asks (Hot Path)");
}
BENCHMARK(BM_OrderBookV2_GetTop5Asks);

/**
 * @brief Benchmark: OrderBook V2 - getBidLevels (cold path, 20 levels)
 * Target: <50ns
 */
static void BM_OrderBookV2_GetBidLevels(benchmark::State& state) {
    OrderBookV2 book;
    populateOrderBook(book, 20);
    
    for (auto _ : state) {
        auto levels = book.getBidLevels(20);
        benchmark::DoNotOptimize(levels);
    }
    
    state.SetLabel("V2 Cold Path (20 levels)");
}
BENCHMARK(BM_OrderBookV2_GetBidLevels);

/**
 * @brief Benchmark: OrderBook V2 - getAskLevels (cold path, 20 levels)
 * Target: <50ns
 */
static void BM_OrderBookV2_GetAskLevels(benchmark::State& state) {
    OrderBookV2 book;
    populateOrderBook(book, 20);
    
    for (auto _ : state) {
        auto levels = book.getAskLevels(20);
        benchmark::DoNotOptimize(levels);
    }
    
    state.SetLabel("V2 Cold Path (20 levels)");
}
BENCHMARK(BM_OrderBookV2_GetAskLevels);

/**
 * @brief Benchmark: OrderBook V2 - update (single level)
 * Target: <100ns
 */
static void BM_OrderBookV2_Update(benchmark::State& state) {
    OrderBookV2 book;
    populateOrderBook(book, 20);
    
    const size_t TICK_COUNT = 131072;
    const size_t MASK = TICK_COUNT - 1;
    auto ticks = generateRandomTicks(TICK_COUNT);
    size_t idx = 0;
    
    for (auto _ : state) {
        book.update(ticks[idx]);
        idx = (idx + 1) & MASK;
        benchmark::DoNotOptimize(book);
    }
    
    state.SetLabel("V2 Update");
}
BENCHMARK(BM_OrderBookV2_Update);

// ============================================================================
// Comparison Benchmarks (V1 vs V2)
// ============================================================================

/**
 * @brief Benchmark: Compare V1 vs V2 - Best Bid/Ask access
 */
static void BM_Compare_BestBidAsk_V1(benchmark::State& state) {
    OrderBook book;
    populateOrderBook(book, 20);
    
    for (auto _ : state) {
        auto bid = book.getBestBid();
        auto ask = book.getBestAsk();
        benchmark::DoNotOptimize(bid);
        benchmark::DoNotOptimize(ask);
    }
}
BENCHMARK(BM_Compare_BestBidAsk_V1);

static void BM_Compare_BestBidAsk_V2(benchmark::State& state) {
    OrderBookV2 book;
    populateOrderBook(book, 20);
    
    for (auto _ : state) {
        auto bid = book.getBestBid();
        auto ask = book.getBestAsk();
        benchmark::DoNotOptimize(bid);
        benchmark::DoNotOptimize(ask);
    }
}
BENCHMARK(BM_Compare_BestBidAsk_V2);

/**
 * @brief Benchmark: OrderBook V2 - getBidLevelsFast (stack-allocated cold path)
 * Target: <50ns
 */
static void BM_OrderBookV2_GetBidLevelsFast(benchmark::State& state) {
    OrderBookV2 book;
    populateOrderBook(book, 20);

    for (auto _ : state) {
        auto [levels, count] = book.getBidLevelsFast();
        benchmark::DoNotOptimize(levels);
        benchmark::DoNotOptimize(count);
    }

    state.SetLabel("V2 Cold Path Fast (no heap alloc)");
}
BENCHMARK(BM_OrderBookV2_GetBidLevelsFast);

/**
 * @brief Benchmark: OrderBook V2 - getAskLevelsFast (stack-allocated cold path)
 * Target: <50ns
 */
static void BM_OrderBookV2_GetAskLevelsFast(benchmark::State& state) {
    OrderBookV2 book;
    populateOrderBook(book, 20);

    for (auto _ : state) {
        auto [levels, count] = book.getAskLevelsFast();
        benchmark::DoNotOptimize(levels);
        benchmark::DoNotOptimize(count);
    }

    state.SetLabel("V2 Cold Path Fast (no heap alloc)");
}
BENCHMARK(BM_OrderBookV2_GetAskLevelsFast);

/**
 * @brief Benchmark: Compare V1 vs V2 - Full depth access
 */
static void BM_Compare_FullDepth_V1(benchmark::State& state) {
    OrderBook book;
    populateOrderBook(book, 20);
    
    for (auto _ : state) {
        auto bids = book.getBidLevels(20);
        auto asks = book.getAskLevels(20);
        benchmark::DoNotOptimize(bids);
        benchmark::DoNotOptimize(asks);
    }
}
BENCHMARK(BM_Compare_FullDepth_V1);

static void BM_Compare_FullDepth_V2(benchmark::State& state) {
    OrderBookV2 book;
    populateOrderBook(book, 20);
    
    for (auto _ : state) {
        auto bids = book.getBidLevels(20);
        auto asks = book.getAskLevels(20);
        benchmark::DoNotOptimize(bids);
        benchmark::DoNotOptimize(asks);
    }
}
BENCHMARK(BM_Compare_FullDepth_V2);

// ============================================================================
// Realistic Workload Benchmarks
// ============================================================================

/**
 * @brief Benchmark: Realistic mixed workload (90% reads, 10% updates)
 */
static void BM_OrderBookV2_MixedWorkload(benchmark::State& state) {
    OrderBookV2 book;
    populateOrderBook(book, 20);
    
    const size_t OP_COUNT = 131072;
    const size_t MASK = OP_COUNT - 1;
    auto ticks = generateRandomTicks(OP_COUNT);
    
    // Pre-generate operation types (90% reads, 10% updates) to avoid RNG in loop
    std::vector<bool> is_read(OP_COUNT);
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(0, 9);
    for (size_t i = 0; i < OP_COUNT; ++i) {
        is_read[i] = (dist(rng) < 9);
    }
    
    size_t idx = 0;
    for (auto _ : state) {
        if (is_read[idx]) {
            // 90% reads
            auto bid = book.getBestBid();
            auto ask = book.getBestAsk();
            benchmark::DoNotOptimize(bid);
            benchmark::DoNotOptimize(ask);
        } else {
            // 10% updates
            book.update(ticks[idx]);
            benchmark::DoNotOptimize(book);
        }
        idx = (idx + 1) & MASK;
    }
    
    state.SetLabel("V2 Mixed (90% read, 10% update)");
}
BENCHMARK(BM_OrderBookV2_MixedWorkload);

/**
 * @brief Benchmark: High-frequency update scenario
 */
static void BM_OrderBookV2_HighFrequencyUpdates(benchmark::State& state) {
    OrderBookV2 book;
    populateOrderBook(book, 20);
    
    // Use 131072 random ticks instead of 100 sequential ones to completely defeat prefetcher
    const size_t TICK_COUNT = 131072;
    const size_t MASK = TICK_COUNT - 1;
    auto ticks = generateRandomTicks(TICK_COUNT);
    
    size_t idx = 0;
    for (auto _ : state) {
        book.update(ticks[idx]);
        idx = (idx + 1) & MASK;
        benchmark::DoNotOptimize(book);
    }
    
    state.SetLabel("V2 High-Frequency Updates");
}
BENCHMARK(BM_OrderBookV2_HighFrequencyUpdates);

/**
 * @brief Benchmark: Cache performance - sequential access
 */
static void BM_OrderBookV2_CachePerformance(benchmark::State& state) {
    OrderBookV2 book;
    populateOrderBook(book, 20);
    
    for (auto _ : state) {
        // Access hot data multiple times (should stay in L1 cache)
        for (int i = 0; i < 10; ++i) {
            auto bid = book.getBestBid();
            auto ask = book.getBestAsk();
            benchmark::DoNotOptimize(bid);
            benchmark::DoNotOptimize(ask);
        }
    }
    
    state.SetLabel("V2 Cache Performance (10x hot access)");
}
BENCHMARK(BM_OrderBookV2_CachePerformance);

// ============================================================================
// Main
// ============================================================================

BENCHMARK_MAIN();
