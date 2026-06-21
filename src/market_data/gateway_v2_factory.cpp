#include <chronos/market_data/gateway_v2_factory.hpp>
#include <chronos/market_data/adapters/thin_mux_adapter.hpp>
#include <chronos/io/transports/websocket_tls_transport.hpp>
#include <chronos/io/protocols/binance_json_protocol.hpp>
#include <chronos/io/protocols/okx_json_protocol.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cctype>

namespace chronos {
namespace market_data {

AnyGateway createGatewayV2(const std::string& name,
                           const ExchangeConfig& cfg,
                           utils::MPMCQueue<Tick, 65536>& queue) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    // 1. Select protocol (substring match — "binance_testnet" → binance)
    std::unique_ptr<Protocol> protocol;
    if (lower.find("binance") != std::string::npos) {
        protocol = std::make_unique<BinanceJsonProtocol>();
    } else if (lower.find("okx") != std::string::npos) {
        protocol = std::make_unique<OKXJsonProtocol>();
    } else {
        spdlog::error("Unknown gateway name: '{}' (expected binance/okx)", name);
        return {}; // empty AnyGateway
    }

    // 2. Create transport (WebSocketTlsTransport handles TLS + proxy)
    auto transport = std::make_unique<WebSocketTlsTransport>();

    // 3. Compose adapter
    ThinMuxAdapter adapter(std::move(transport), std::move(protocol));

    // 4. Initialize (sets config, queue, heartbeat interval)
    if (!adapter.initialize(cfg, queue)) {
        spdlog::error("GatewayV2 factory: initialize failed for '{}'", name);
        return {};
    }

    spdlog::info("GatewayV2 created: {}", name);
    return AnyGateway(std::move(adapter));
}

} // namespace market_data
} // namespace chronos
