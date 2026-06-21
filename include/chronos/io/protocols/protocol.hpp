#pragma once

#include <chronos/core/types.hpp>
#include <functional>
#include <string>
#include <vector>

namespace chronos {
namespace market_data {

/// Abstract wire-protocol parser for a specific exchange.
///
/// Protocol is stateless — it maps raw text messages to Tick structs and
/// generates wire-format subscription requests.  The owning ThinMuxAdapter
/// provides the symbol→id resolver callback (so Protocol doesn't need to
/// know about symbol registries) and the tick sink (GatewayContext::pushTick).
///
/// A new exchange typically requires *only* a new Protocol implementation
/// (~100-150 lines); the Transport and Adapter layers are reused.
struct Protocol {
    virtual ~Protocol() = default;

    /// Parse one raw message.  Calls onTick for each Tick produced.
    /// Returns the number of ticks emitted (0 for control/error messages).
    /// resolveSymbolId: exchange-formatted symbol → internal uint32_t id.
    virtual size_t parse(const std::string& msg,
                         uint64_t receive_ts,
                         std::function<void(Tick&&)> onTick,
                         std::function<uint32_t(const std::string&)> resolveSymbolId) = 0;

    /// Generate the wire-format JSON for subscribe / unsubscribe requests.
    virtual std::string subscribeRequest(const std::vector<std::string>& symbols) = 0;
    virtual std::string unsubscribeRequest(const std::vector<std::string>& symbols) = 0;

    /// True if msg is an application-level ping (e.g. OKX "ping" text).
    virtual bool isPingMessage(const std::string& msg) const = 0;

    /// True if msg is an application-level pong (e.g. OKX "pong" text).
    virtual bool isPongMessage(const std::string& msg) const = 0;

    /// Payload for an application-level heartbeat (empty if using WS ping frames).
    virtual std::string heartbeatPayload() const = 0;

    /// True when the heartbeat is done via WS ping frames (not text).
    virtual bool usesWsPing() const = 0;

    /// Normalize a user-facing symbol (e.g. "btc_usdt") to exchange format.
    virtual std::string normalizeSymbol(const std::string& raw) const = 0;

    /// Default exchange URL for this protocol (used when config doesn't specify).
    virtual std::string defaultUrl() const = 0;
};

} // namespace market_data
} // namespace chronos
