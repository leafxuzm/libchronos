/**
 * @file benchmark_mpmc_queue.cpp
 * @brief Performance benchmarks for MPMC queue (target: push/pop <50ns each)
 */

#include <benchmark/benchmark.h>
#include <chronos/utils/mpmc_queue.hpp>
#include <thread>
#include <vector>
#include <atomic>

using namespace chronos::utils;

// Simple POD for benchmarking
struct alignas(64) TestMessage {
    uint64_t id;
    uint64_t timestamp;
    double price;
    double quantity;
};

// ============================================================================
// Single-Threaded (Producer + Consumer in same thread — measures raw push/pop)
// ============================================================================

static void BM_MPMCQueue_Push(benchmark::State& state) {
    MPMCQueue<TestMessage, 1024> queue;

    TestMessage msg{42, 1000000, 50000.0, 1.0};

    for (auto _ : state) {
        bool ok = queue.try_push(msg);
        benchmark::DoNotOptimize(ok);
    }

    state.SetLabel("push()");
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MPMCQueue_Push)->Name("BM_MPMCQueue_Push");

static void BM_MPMCQueue_Pop(benchmark::State& state) {
    MPMCQueue<TestMessage, 1024> queue;

    // Pre-fill the queue
    TestMessage msg{42, 1000000, 50000.0, 1.0};
    for (size_t i = 0; i < 1024; ++i) {
        queue.try_push(msg);
    }

    for (auto _ : state) {
        TestMessage out;
        bool ok = queue.try_pop(out);
        benchmark::DoNotOptimize(ok);
        benchmark::DoNotOptimize(out);
    }

    state.SetLabel("pop()");
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MPMCQueue_Pop)->Name("BM_MPMCQueue_Pop");

static void BM_MPMCQueue_PushPop_Paired(benchmark::State& state) {
    MPMCQueue<TestMessage, 1024> queue;

    TestMessage msg{42, 1000000, 50000.0, 1.0};

    for (auto _ : state) {
        queue.try_push(msg);
        TestMessage out;
        queue.try_pop(out);
        benchmark::DoNotOptimize(out);
    }

    state.SetLabel("push+pop pair");
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MPMCQueue_PushPop_Paired)->Name("BM_MPMCQueue_PushPop_Paired");

// ============================================================================
// SPSC Latency (round-trip: producer → consumer → producer)
// ============================================================================

static void BM_MPMCQueue_SPSC_Latency(benchmark::State& state) {
    constexpr size_t TOTAL = 100000;

    for (auto _ : state) {
        MPMCQueue<TestMessage, 1024> queue;
        std::atomic<bool> start{false};

        std::thread producer([&]() {
            while (!start.load(std::memory_order_relaxed)) {}
            TestMessage msg{};
            for (size_t i = 0; i < TOTAL; ++i) {
                msg.id = i;
                while (!queue.try_push(msg)) {}
            }
        });

        std::thread consumer([&]() {
            while (!start.load(std::memory_order_relaxed)) {}
            TestMessage out;
            for (size_t i = 0; i < TOTAL; ++i) {
                while (!queue.try_pop(out)) {}
            }
        });

        auto t_start = std::chrono::high_resolution_clock::now();
        start.store(true, std::memory_order_release);

        producer.join();
        consumer.join();
        auto t_end = std::chrono::high_resolution_clock::now();

        auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            t_end - t_start).count();
        double per_msg_ns = static_cast<double>(elapsed_ns) / static_cast<double>(TOTAL);

        state.counters["total_msgs"] = static_cast<double>(TOTAL);
        state.counters["total_time_ns"] = static_cast<double>(elapsed_ns);
        state.counters["per_msg_ns"] = per_msg_ns;
        state.SetItemsProcessed(TOTAL);
    }
    state.SetLabel("SPSC round-trip latency");
}
BENCHMARK(BM_MPMCQueue_SPSC_Latency)->Name("BM_MPMCQueue_SPSC_Latency")
    ->Unit(benchmark::kNanosecond);

// ============================================================================
// MPMC Throughput (4 producers, 4 consumers)
// ============================================================================

static void BM_MPMCQueue_MPMC_Throughput(benchmark::State& state) {
    constexpr size_t MSGS_PER_PRODUCER = 50000;
    constexpr int NUM_PRODUCERS = 4;
    constexpr int NUM_CONSUMERS = 4;

    for (auto _ : state) {
        MPMCQueue<TestMessage, 4096> queue;
        std::atomic<bool> start{false};

        std::vector<std::thread> producers;
        std::vector<std::thread> consumers;

        for (int i = 0; i < NUM_PRODUCERS; ++i) {
            producers.emplace_back([&, i]() {
                while (!start.load(std::memory_order_relaxed)) {}
                TestMessage msg{};
                msg.price = 50000.0 + i;
                for (size_t j = 0; j < MSGS_PER_PRODUCER; ++j) {
                    msg.id = (static_cast<uint64_t>(i) << 32) | j;
                    while (!queue.try_push(msg)) {}
                }
            });
        }

        for (int i = 0; i < NUM_CONSUMERS; ++i) {
            consumers.emplace_back([&]() {
                while (!start.load(std::memory_order_relaxed)) {}
                TestMessage out;
                size_t local_consumed = 0;
                size_t target = MSGS_PER_PRODUCER * NUM_PRODUCERS / NUM_CONSUMERS;
                while (local_consumed < target) {
                    if (queue.try_pop(out)) {
                        benchmark::DoNotOptimize(out);
                        ++local_consumed;
                    }
                }
            });
        }

        auto t_start = std::chrono::high_resolution_clock::now();
        start.store(true, std::memory_order_release);

        for (auto& t : producers) t.join();
        for (auto& t : consumers) t.join();
        auto t_end = std::chrono::high_resolution_clock::now();

        auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            t_end - t_start).count();
        size_t total = MSGS_PER_PRODUCER * NUM_PRODUCERS;
        double per_msg_ns = static_cast<double>(elapsed_ns) / static_cast<double>(total);

        state.counters["total_msgs"] = static_cast<double>(total);
        state.counters["throughput_msg_per_sec"] = total * 1e9 / static_cast<double>(elapsed_ns);
        state.counters["per_msg_ns"] = per_msg_ns;
        state.SetItemsProcessed(total);
    }
    state.SetLabel("4P4C throughput");
}
BENCHMARK(BM_MPMCQueue_MPMC_Throughput)->Name("BM_MPMCQueue_MPMC_Throughput")
    ->Unit(benchmark::kNanosecond);

// BENCHMARK_MAIN defined in benchmark_orderbook.cpp
