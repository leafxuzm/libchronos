/**
 * @file benchmark_risk_engine.cpp
 * @brief Performance benchmarks for RiskEngine (target: checkOrder <1μs, checkRateLimit <50ns)
 */

#include <benchmark/benchmark.h>
#include <chronos/risk/risk_engine.hpp>
#include <chronos/trading/position_manager.hpp>
#include <chronos/core/types.hpp>
#include <atomic>
#include <thread>
#include <vector>

using namespace chronos;
using namespace chronos::risk;
using namespace chronos::trading;

namespace {

OrderRequest makeOrder(uint32_t symbol_id, double price, double qty,
                       OrderSide side = OrderSide::BUY) {
    OrderRequest o;
    o.order_id = 1;
    o.symbol_id = symbol_id;
    o.price = toDecimal(price);
    o.quantity = toDecimal(qty);
    o.side = side;
    return o;
}

Fill makeFill(uint32_t symbol_id, double price, double qty,
              OrderSide side = OrderSide::BUY) {
    Fill f;
    f.symbol_id = symbol_id;
    f.fill_price = toDecimal(price);
    f.fill_quantity = toDecimal(qty);
    f.side = side;
    return f;
}

} // anonymous namespace

// ============================================================================
// Single-Threaded Benchmarks
// ============================================================================

static void BM_RiskEngine_CheckOrder(benchmark::State& state) {
    PositionManager pm;
    pm.updatePosition(makeFill(1, 50000.0, 0.5));
    RiskEngine engine(pm);

    RiskParameters wide;
    wide.max_order_value = 1000000.0;
    wide.max_orders_per_second = 1000000;
    wide.max_position_value = 1000000.0;
    wide.max_total_position_value = 10000000.0;
    engine.updateParameters(wide);

    auto order = makeOrder(1, 50000.0, 0.1);

    for (auto _ : state) {
        auto result = engine.checkOrder(order);
        benchmark::DoNotOptimize(result);
    }

    state.SetLabel("checkOrder() — hot path");
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RiskEngine_CheckOrder)->Name("BM_RiskEngine_CheckOrder");

static void BM_RiskEngine_CheckRateLimit(benchmark::State& state) {
    PositionManager pm;
    RiskEngine engine(pm);

    RiskParameters wide;
    wide.max_orders_per_second = 100000000;
    engine.updateParameters(wide);

    for (auto _ : state) {
        bool ok = engine.checkRateLimit();
        benchmark::DoNotOptimize(ok);
    }

    state.SetLabel("checkRateLimit() — lightweight pre-filter");
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RiskEngine_CheckRateLimit)->Name("BM_RiskEngine_CheckRateLimit");

// ============================================================================
// Multi-Threaded Benchmarks
// ============================================================================

static void BM_RiskEngine_CheckOrder_2Threads(benchmark::State& state) {
    PositionManager pm;
    pm.updatePosition(makeFill(1, 50000.0, 0.5));
    RiskEngine engine(pm);

    RiskParameters wide;
    wide.max_order_value = 1000000.0;
    wide.max_orders_per_second = 10000000;
    wide.max_position_value = 1000000.0;
    wide.max_total_position_value = 10000000.0;
    engine.updateParameters(wide);

    for (auto _ : state) {
        std::atomic<bool> start{false};
        std::atomic<size_t> done{0};

        auto worker = [&]() {
            while (!start.load(std::memory_order_relaxed)) {}
            auto order = makeOrder(1, 50000.0, 0.01);
            for (int i = 0; i < 100000; ++i) {
                benchmark::DoNotOptimize(engine.checkOrder(order));
            }
            done.fetch_add(1, std::memory_order_relaxed);
        };

        std::thread t1(worker);
        std::thread t2(worker);

        auto t_start = std::chrono::high_resolution_clock::now();
        start.store(true, std::memory_order_release);

        while (done.load(std::memory_order_relaxed) < 2) {}

        auto t_end = std::chrono::high_resolution_clock::now();
        t1.join(); t2.join();

        auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            t_end - t_start).count();
        state.SetIterationTime(static_cast<double>(elapsed_ns) / 200000.0);
        state.SetItemsProcessed(200000);
    }
}
BENCHMARK(BM_RiskEngine_CheckOrder_2Threads)
    ->Name("BM_RiskEngine_CheckOrder_2Threads")
    ->UseManualTime()
    ->Iterations(1)
    ;

static void BM_RiskEngine_CheckOrder_4Threads(benchmark::State& state) {
    PositionManager pm;
    pm.updatePosition(makeFill(1, 50000.0, 0.5));
    RiskEngine engine(pm);

    RiskParameters wide;
    wide.max_order_value = 1000000.0;
    wide.max_orders_per_second = 10000000;
    wide.max_position_value = 1000000.0;
    wide.max_total_position_value = 10000000.0;
    engine.updateParameters(wide);

    for (auto _ : state) {
        std::atomic<bool> start{false};
        std::atomic<size_t> done{0};

        auto worker = [&]() {
            while (!start.load(std::memory_order_relaxed)) {}
            auto order = makeOrder(1, 50000.0, 0.01);
            for (int i = 0; i < 50000; ++i) {
                benchmark::DoNotOptimize(engine.checkOrder(order));
            }
            done.fetch_add(1, std::memory_order_relaxed);
        };

        std::thread t1(worker);
        std::thread t2(worker);
        std::thread t3(worker);
        std::thread t4(worker);

        auto t_start = std::chrono::high_resolution_clock::now();
        start.store(true, std::memory_order_release);

        while (done.load(std::memory_order_relaxed) < 4) {}

        auto t_end = std::chrono::high_resolution_clock::now();
        t1.join(); t2.join(); t3.join(); t4.join();

        auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            t_end - t_start).count();
        state.SetIterationTime(static_cast<double>(elapsed_ns) / 200000.0);
        state.SetItemsProcessed(200000);
    }
}
BENCHMARK(BM_RiskEngine_CheckOrder_4Threads)
    ->Name("BM_RiskEngine_CheckOrder_4Threads")
    ->UseManualTime()
    ->Iterations(1)
    ;

// BENCHMARK_MAIN defined in benchmark_orderbook.cpp
