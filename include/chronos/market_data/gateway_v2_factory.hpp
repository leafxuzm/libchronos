#pragma once

#include <chronos/market_data/any_gateway.hpp>
#include <chronos/core/config.hpp>
#include <chronos/utils/mpmc_queue.hpp>
#include <memory>
#include <string>

namespace chronos {
namespace market_data {

/// Assemble Transport + Protocol + ThinMuxAdapter → AnyGateway.
/// Returns a ready-to-use (initialized but not started) gateway.
AnyGateway createGatewayV2(const std::string& name,
                           const ExchangeConfig& cfg,
                           utils::MPMCQueue<Tick, 65536>& queue);

} // namespace market_data
} // namespace chronos
