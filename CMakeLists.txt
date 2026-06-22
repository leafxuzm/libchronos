cmake_minimum_required(VERSION 3.20)
project(libchronos VERSION 1.0.0 LANGUAGES CXX
        DESCRIPTION "Core library for Chronos HFT platform")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# CMake 4.x compatibility
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# ── Build options ────────────────────────────────────────────────
option(BUILD_TESTS "Build test suite" ON)
option(BUILD_BENCHMARKS "Build performance benchmarks" ON)
option(CHRONOS_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(CHRONOS_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(CHRONOS_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)

# ── Compiler flags ───────────────────────────────────────────────
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -Wall -Wextra -Wpedantic")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native -mtune=native")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g -DNDEBUG")

# ── Dependencies (must come before sanitizer flags) ──────────────
# chronos_deps provides: fpm, yaml-cpp, gflags, spdlog, nlohmann_json,
# simdjson, boost_headers, OpenSSL, libzmq — all via FetchContent
# with system-package fallbacks.
# Placed BEFORE sanitizer flags so libzmq's try_compile for poll()
# detection is not polluted by -fsanitize=address.
add_subdirectory(${CMAKE_SOURCE_DIR}/../libchronos-deps
                 ${CMAKE_BINARY_DIR}/chronos_deps)

# Sanitizer flags: global so tests/benchmarks also inherit them.
# Applied AFTER deps so third-party detection is clean.
if(CHRONOS_ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()
if(CHRONOS_ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()
if(CHRONOS_ENABLE_UBSAN)
    add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=undefined)
endif()

# ── Core library sources (always built) ──────────────────────────
set(CHRONOS_SOURCES
    src/core/types.cpp
    src/core/parse_decimal.cpp
    src/core/config.cpp
    src/core/error.cpp
    src/market_data/orderbook.cpp
    src/market_data/orderbook_v2.cpp
    src/trading/position_manager.cpp
    src/trading/strategy_engine.cpp
    src/risk/risk_engine.cpp
    src/execution/order_gateway.cpp
    src/execution/execution_handler.cpp
    src/logging/log_writer.cpp
    src/logging/log_reader.cpp
    src/logging/zmq_bridge.cpp
    src/backtest/time_replayer.cpp
    src/backtest/metrics.cpp
    src/backtest/backtest_engine.cpp
    src/monitoring/health_monitor.cpp
    src/io/security/security.cpp
)

# ── IO-layer sources (require Boost + OpenSSL) ───────────────────
# chronos_deps guarantees these are available (or build fails at configure).
list(APPEND CHRONOS_SOURCES
    src/market_data/gateway.cpp
    src/io/transports/websocket_tls_transport.cpp
    src/io/protocols/binance_json_protocol.cpp
    src/io/protocols/okx_json_protocol.cpp
    src/market_data/adapters/binance_adapter.cpp
    src/market_data/adapters/okx_adapter.cpp
    src/market_data/adapters/thin_mux_adapter.cpp
    src/market_data/gateway_v2_factory.cpp
    src/execution/binance_http_client.cpp
    src/execution/binance_user_stream.cpp
    src/io/security/hmac_sha256.cpp
)
message(STATUS "IO layer: enabled (Boost.Beast + OpenSSL via chronos_deps)")

# ── Static library ───────────────────────────────────────────────
add_library(chronos STATIC ${CHRONOS_SOURCES})

target_include_directories(chronos
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(chronos
    PUBLIC
        chronos_deps
)

# Signal that IO layer is available (consumers can #ifdef on this)
target_compile_definitions(chronos PUBLIC CHRONOS_HAS_LIVE_MODE)

target_compile_features(chronos PUBLIC cxx_std_20)

# GCC warns about fpm fixed.hpp constexpr comparisons that are tautological
# by design (e.g., range checks on fixed-point types).
target_compile_options(chronos PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wno-tautological-compare>
)

# ── Install ──────────────────────────────────────────────────────
install(TARGETS chronos
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
    INCLUDES DESTINATION include
)
install(DIRECTORY include/chronos
    DESTINATION include
    FILES_MATCHING PATTERN "*.hpp"
)

# ── Tests ────────────────────────────────────────────────────────
if(BUILD_TESTS)
    enable_testing()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
        add_subdirectory(tests)
    endif()
endif()

# ── Benchmarks ───────────────────────────────────────────────────
if(BUILD_BENCHMARKS)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/benchmark/CMakeLists.txt")
        add_subdirectory(benchmark)
    endif()
endif()

# ── Configuration summary ────────────────────────────────────────
message(STATUS "")
message(STATUS "libchronos Configuration Summary:")
message(STATUS "  Version:           ${PROJECT_VERSION}")
message(STATUS "  Build type:        ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard:      C++${CMAKE_CXX_STANDARD}")
message(STATUS "  Build tests:       ${BUILD_TESTS}")
message(STATUS "  Build benchmarks:  ${BUILD_BENCHMARKS}")
message(STATUS "  IO layer (TLS):    ON (via chronos_deps)")
message(STATUS "")
