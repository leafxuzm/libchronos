# Chronos Performance Benchmarks

add_executable(chronos_benchmarks
    benchmark_orderbook.cpp
    benchmark_order_id_generator.cpp
    benchmark_risk_engine.cpp
    benchmark_mpmc_queue.cpp
    benchmark_e2e_latency.cpp
)

target_link_libraries(chronos_benchmarks
    PRIVATE
        chronos
        benchmark::benchmark
        benchmark::benchmark_main
)

target_compile_options(chronos_benchmarks PRIVATE
    -O3
    -march=native
    -mtune=native
    -DNDEBUG
)

add_test(NAME chronos_benchmarks COMMAND chronos_benchmarks --benchmark_min_time=0.1)

message(STATUS "Benchmark suite configured")
