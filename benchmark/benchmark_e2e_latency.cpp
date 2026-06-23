/**
 * @file benchmark_e2e_latency.cpp
 * @brief End-to-end latency & throughput benchmarks for the full trading pipeline
 *
 * Measures:
 *   - Tick push → Order pop latency (tick_queue → engine → strategy → order_queue)
 *   - Tick processing throughput (ticks/sec sustained)
 *
 * Targets: tick→decision <50μs P99, >1M ticks/sec throughput
 */

#include <benchmark/benchmark.h>
#include <chronos/trading/strategy_engine.hpp>
#include <chronos/core/types.hpp>
#include <thread>
#include <atomic>

using namespace chronos;
using namespace chronos::trading;

// Minimal strategy that always submits a BUY order on every tick.
// This keeps the strategy path as short as possible for pipeline measurement.
class BenchmarkStrategy : public Strategy {
public:
    const char* name() const override { return "BenchmarkStrategy"; }

    std::vector<uint32_t> symbols() const override { return {}; }  // wildcard

    void onTick(const Tick& tick, StrategyContext& ctx) override {
        ticks_processed.fetch_add(1, std::memory_order_relaxed);

        OrderRequest order;
        order.symbol_id = tick.symbol_id;
        order.price      = tick.price;
        order.quantity   = toDecimal(0.01);
        order.side       = OrderSide::BUY;
        order.type       = OrderType::LIMIT;

        uint64_t id = ctx.submitOrder(order);
        if (id) orders_submitted.fetch_add(1, std::memory_order_relaxed);
    }

    std::atomic<uint64_t> ticks_processed{0};
    std::atomic<uint64_t> orders_submitted{0};
};

// ============================================================================
// E2E Latency: pushTick → popOrder (single tick, hot path)
// ============================================================================

static void BM_E2E_TickToOrder_Latency(benchmark::State& state) {
    RiskParameters params;
    params.max_order_value = 1000000.0;
    params.max_orders_per_second = 10000000;

    for (auto _ : state) {
        state.PauseTiming();
        StrategyEngine engine;
        engine.setAvailableCapital(toDecimal(100000.0));
        engine.updateRiskParameters(params);

        auto strategy = std::make_unique<BenchmarkStrategy>();
        engine.registerStrategy(std::move(strategy));
        engine.start();

        // Drain any orders from onLoad
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        OrderRequest dummy;
        while (engine.popOrder(dummy)) {}
        state.ResumeTiming();

        Tick tick;
        tick.symbol_id = 1;
        tick.price = toDecimal(50000.0);
        tick.quantity = toDecimal(1.0);
        tick.side = TickSide::BID;
        tick.receive_timestamp_us = 1000000;

        auto start = std::chrono::high_resolution_clock::now();
        engine.pushTick(tick);

        OrderRequest out;
        while (!engine.popOrder(out)) {
            // spin-wait — engine processes on background thread
        }
        auto end = std::chrono::high_resolution_clock::now();

        auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        state.SetIterationTime(static_cast<double>(elapsed_ns) / 1e9);

        engine.stop();
    }

    state.SetLabel("pushTick→popOrder");
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_E2E_TickToOrder_Latency)
    ->Name("BM_E2E_TickToOrder_Latency")
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

// ============================================================================
// E2E Latency: tick → decision (strategy onTick entry → submitOrder return)
// ============================================================================

static void BM_E2E_StrategyDecision_Latency(benchmark::State& state) {
    RiskParameters params;
    params.max_order_value = 1000000.0;

    for (auto _ : state) {
        state.PauseTiming();
        StrategyEngine engine;
        engine.setAvailableCapital(toDecimal(100000.0));
        engine.updateRiskParameters(params);

        auto strategy = std::make_unique<BenchmarkStrategy>();
        auto* raw = strategy.get();
        engine.registerStrategy(std::move(strategy));
        engine.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

        OrderRequest dummy;
        while (engine.popOrder(dummy)) {}
        state.ResumeTiming();

        Tick tick;
        tick.symbol_id = 1;
        tick.price = toDecimal(50000.0);
        tick.quantity = toDecimal(1.0);
        tick.side = TickSide::BID;
        tick.receive_timestamp_us = 1000000;

        // Push + wait for engine to process + drain order
        engine.pushTick(tick);

        OrderRequest out;
        while (!engine.popOrder(out)) {}

        auto decisions = raw->ticks_processed.load(std::memory_order_relaxed);
        benchmark::DoNotOptimize(decisions);
    }

    state.SetLabel("strategy decisions");
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_E2E_StrategyDecision_Latency)
    ->Name("BM_E2E_StrategyDecision_Latency")
    ->Unit(benchmark::kNanosecond);

// ============================================================================
// Throughput: sustained tick processing rate (batch of 1000 ticks)
// ============================================================================

static void BM_E2E_Throughput_1k_Ticks(benchmark::State& state) {
    constexpr size_t BATCH_SIZE = 1000;

    RiskParameters params;
    params.max_order_value = 1000000.0;
    params.max_orders_per_second = 10000000;

    for (auto _ : state) {
        state.PauseTiming();
        StrategyEngine engine;
        engine.setAvailableCapital(toDecimal(100000.0));
        engine.updateRiskParameters(params);

        auto strategy = std::make_unique<BenchmarkStrategy>();
        auto* raw = strategy.get();
        engine.registerStrategy(std::move(strategy));
        engine.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        state.ResumeTiming();

        // Push batch of ticks
        auto t_start = std::chrono::high_resolution_clock::now();
        for (size_t i = 0; i < BATCH_SIZE; ++i) {
            Tick tick;
            tick.symbol_id = 1;
            tick.price = toDecimal(50000.0 + (i % 100) * 0.01);
            tick.quantity = toDecimal(1.0);
            tick.side = TickSide::BID;
            tick.receive_timestamp_us = 1000000 + i;
            engine.pushTick(tick);
        }

        // Drain all orders (spin until all ticks processed)
        size_t orders_drained = 0;
        OrderRequest out;
        while (orders_drained < BATCH_SIZE) {
            while (engine.popOrder(out)) {
                benchmark::DoNotOptimize(out);
                orders_drained++;
            }
        }
        auto t_end = std::chrono::high_resolution_clock::now();

        auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count();
        double ticks_per_sec = BATCH_SIZE * 1e9 / static_cast<double>(elapsed_ns);

        state.counters["batch_size"] = static_cast<double>(BATCH_SIZE);
        state.counters["ticks_per_sec"] = ticks_per_sec;
        state.counters["per_tick_ns"] = static_cast<double>(elapsed_ns) / BATCH_SIZE;
        state.SetItemsProcessed(BATCH_SIZE);

        engine.stop();
    }

    state.SetLabel("throughput");
}
BENCHMARK(BM_E2E_Throughput_1k_Ticks)
    ->Name("BM_E2E_Throughput_1k_Ticks")
    ->Unit(benchmark::kNanosecond);

// ============================================================================
// Throughput: pipeline stages — measure queue depth under load
// ============================================================================

static void BM_E2E_Throughput_Sustained(benchmark::State& state) {
    constexpr size_t TOTAL_TICKS = 10000;
    constexpr size_t BURST_SIZE = 100;

    RiskParameters params;
    params.max_order_value = 1000000.0;
    params.max_orders_per_second = 10000000;

    for (auto _ : state) {
        state.PauseTiming();
        StrategyEngine engine;
        engine.setAvailableCapital(toDecimal(100000.0));
        engine.updateRiskParameters(params);

        auto strategy = std::make_unique<BenchmarkStrategy>();
        engine.registerStrategy(std::move(strategy));
        engine.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

        // Drain initial
        OrderRequest dummy;
        while (engine.popOrder(dummy)) {}
        state.ResumeTiming();

        size_t pushed = 0;
        size_t drained = 0;
        auto t_start = std::chrono::high_resolution_clock::now();

        while (pushed < TOTAL_TICKS) {
            // Burst push
            for (size_t i = 0; i < BURST_SIZE && pushed < TOTAL_TICKS; ++i) {
                Tick tick;
                tick.symbol_id = 1;
                tick.price = toDecimal(50000.0 + (pushed % 100) * 0.01);
                tick.quantity = toDecimal(1.0);
                tick.side = TickSide::BID;
                tick.receive_timestamp_us = 1000000 + pushed;
                if (!engine.pushTick(tick)) break;
                pushed++;
            }

            // Drain available
            OrderRequest out;
            while (engine.popOrder(out)) {
                drained++;
            }
        }

        // Final drain — yield to let engine thread flush remaining orders
        auto t_drain_start = std::chrono::high_resolution_clock::now();
        while (drained < pushed) {
            OrderRequest out;
            while (engine.popOrder(out)) drained++;
            auto drain_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::high_resolution_clock::now() - t_drain_start).count();
            if (drain_ms > 5000) break;  // 5s timeout prevents infinite spin
            std::this_thread::yield();
        }
        auto t_end = std::chrono::high_resolution_clock::now();

        auto total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count();
        double ticks_per_sec = TOTAL_TICKS * 1e9 / static_cast<double>(total_ns);

        state.counters["total_ticks"] = static_cast<double>(TOTAL_TICKS);
        state.counters["ticks_per_sec"] = ticks_per_sec;
        state.counters["per_tick_ns"] = static_cast<double>(total_ns) / TOTAL_TICKS;
        state.SetItemsProcessed(TOTAL_TICKS);

        engine.stop();
    }

    state.SetLabel("sustained throughput");
}
BENCHMARK(BM_E2E_Throughput_Sustained)
    ->Name("BM_E2E_Throughput_Sustained")
    ->Unit(benchmark::kNanosecond);
