# Unit Tests

add_executable(unit_tests
    test_mpmc_queue.cpp
    test_orderbook_basic.cpp
    test_order_id_generator.cpp
    test_position_manager.cpp
    test_risk_engine.cpp
    test_strategy_engine.cpp
    test_pbt_properties.cpp
    test_order_gateway.cpp
    test_execution_handler.cpp
    test_log_writer.cpp
    test_log_reader.cpp
    test_zmq_bridge.cpp
    test_time_replayer.cpp
    test_metrics.cpp
    test_backtest_engine.cpp
    test_integration_pipeline.cpp
    test_stress.cpp
    test_system.cpp
    test_health_check.cpp
    test_security.cpp
    test_parse_decimal.cpp
    test_binance_connectivity.cpp
    test_okx_connectivity.cpp
    test_stress_multi_exchange.cpp
    test_full_pipeline_perf.cpp
    test_any_gateway.cpp
    test_architecture_comparison.cpp
    test_protocol.cpp
    test_trading_pipeline.cpp
)

target_link_libraries(unit_tests
    chronos
    gtest_main
    gtest
    rapidcheck
    ${CMAKE_THREAD_LIBS_INIT}
)

# RapidCheck gtest integration (extras/gtest/include)
target_include_directories(unit_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/../libchronos-deps/_deps_cache/rapidcheck-src/extras/gtest/include
)

include(GoogleTest)
gtest_discover_tests(unit_tests)

message(STATUS "Unit tests configured (29 files)")
