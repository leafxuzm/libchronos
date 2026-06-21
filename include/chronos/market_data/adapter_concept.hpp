#pragma once

#include <chronos/core/types.hpp>
#include <chronos/core/config.hpp>
#include <chronos/utils/mpmc_queue.hpp>
#include <concepts>
#include <string_view>
#include <cstdint>

namespace chronos {
namespace market_data {

/// Compile-time contract for any exchange adapter.
///
/// Satisfied by ThinMuxAdapter (and any future standalone adapter class).
/// Used by AnyGateway to constrain the type-erased wrapper's constructor.
/// No inheritance required.
template <typename T>
concept ExchangeAdapter = requires(T a, const T ca,
    const ExchangeConfig& cfg, utils::MPMCQueue<Tick, 65536>& queue,
    std::string_view sym) {

    { a.initialize(cfg, queue) } -> std::same_as<bool>;
    { a.start()              } -> std::same_as<bool>;
    { a.stop()               } -> std::same_as<void>;
    { a.subscribe(sym)       } -> std::same_as<bool>;
    { a.unsubscribe(sym)     } -> std::same_as<bool>;
    { ca.isRunning()         } -> std::same_as<bool>;
    { ca.getStatus()         } -> std::same_as<ConnectionStatus>;
    { ca.getStatistics()     } -> std::same_as<GatewayStatistics>;
};

} // namespace market_data
} // namespace chronos
