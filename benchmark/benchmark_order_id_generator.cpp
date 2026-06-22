/**
 * @file benchmark_order_id_generator.cpp
 * @brief Performance benchmarks for OrderIDGenerator (target: <10ns per generate)
 */

#include <benchmark/benchmark.h>
#include <chronos/trading/order_id_generator.hpp>
#include <thread>
#include <vector>
#include <atomic>

using namespace chronos::trading;

// ============================================================================
// Single-Threaded Benchmarks
// ============================================================================

static void BM_OrderID_Generate(benchmark::State& state) {
    OrderIDGenerator gen;
    for (auto _ : state) {
        uint64_t id = gen.generate();
        benchmark::DoNotOptimize(id);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OrderID_Generate)->Name("BM_OrderID_Generate");

static void BM_OrderID_GetCurrentId(benchmark::State& state) {
    OrderIDGenerator gen;
    gen.generate();  // ensure non-zero
    for (auto _ : state) {
        uint64_t id = gen.getCurrentID();
        benchmark::DoNotOptimize(id);
    }
}
BENCHMARK(BM_OrderID_GetCurrentId)->Name("BM_OrderID_GetCurrentId");

// ============================================================================
// Multi-Threaded (Throughput) Benchmarks
// ============================================================================

static void BM_OrderID_Generate_2Threads(benchmark::State& state) {
    OrderIDGenerator gen;

    for (auto _ : state) {
        std::atomic<bool> start{false};
        std::atomic<size_t> done{0};

        auto worker = [&]() {
            while (!start.load(std::memory_order_relaxed)) { /* spin */ }
            for (int i = 0; i < 100000; ++i) {
                benchmark::DoNotOptimize(gen.generate());
            }
            done.fetch_add(1, std::memory_order_relaxed);
        };

        std::thread t1(worker);
        std::thread t2(worker);

        auto t_start = std::chrono::high_resolution_clock::now();
        start.store(true, std::memory_order_release);

        while (done.load(std::memory_order_relaxed) < 2) { /* spin */ }
        auto t_end = std::chrono::high_resolution_clock::now();

        t1.join();
        t2.join();

        auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            t_end - t_start).count();
        state.SetIterationTime(static_cast<double>(elapsed_ns) / 200000.0);
        state.SetItemsProcessed(200000);
    }
}
BENCHMARK(BM_OrderID_Generate_2Threads)
    ->Name("BM_OrderID_Generate_2Threads")
    ->UseManualTime()
    ->Iterations(1)
    ;

static void BM_OrderID_Generate_4Threads(benchmark::State& state) {
    OrderIDGenerator gen;

    for (auto _ : state) {
        std::atomic<bool> start{false};
        std::atomic<size_t> done{0};

        auto worker = [&]() {
            while (!start.load(std::memory_order_relaxed)) { /* spin */ }
            for (int i = 0; i < 50000; ++i) {
                benchmark::DoNotOptimize(gen.generate());
            }
            done.fetch_add(1, std::memory_order_relaxed);
        };

        std::thread t1(worker);
        std::thread t2(worker);
        std::thread t3(worker);
        std::thread t4(worker);

        auto t_start = std::chrono::high_resolution_clock::now();
        start.store(true, std::memory_order_release);

        while (done.load(std::memory_order_relaxed) < 4) { /* spin */ }
        auto t_end = std::chrono::high_resolution_clock::now();

        t1.join(); t2.join(); t3.join(); t4.join();

        auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            t_end - t_start).count();
        state.SetIterationTime(static_cast<double>(elapsed_ns) / 200000.0);
        state.SetItemsProcessed(200000);
    }
}
BENCHMARK(BM_OrderID_Generate_4Threads)
    ->Name("BM_OrderID_Generate_4Threads")
    ->UseManualTime()
    ->Iterations(1)
    ;

// BENCHMARK_MAIN defined in benchmark_orderbook.cpp
