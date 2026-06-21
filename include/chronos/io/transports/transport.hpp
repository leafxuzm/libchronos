#pragma once

#include <chronos/core/config.hpp>
#include <string>

namespace chronos {
namespace market_data {

/// Abstract synchronous transport layer.
///
/// Implementations wrap a wire protocol (WebSocket, raw TCP, UDP multicast)
/// behind a blocking send/receive interface.  The adapter (ThinMuxAdapter)
/// owns one Transport and drives its lifecycle from dedicated threads.
struct Transport {
    virtual ~Transport() = default;

    /// Establish connection.  cfg provides URL, proxy settings, timeouts.
    /// Returns true on success, false (and logs) on failure.
    virtual bool connect(const ExchangeConfig& cfg) = 0;

    /// Gracefully tear down the connection.
    virtual void disconnect() = 0;

    /// Send a text (or binary) frame.  Non-blocking, best-effort.
    virtual bool send(const std::string& msg) = 0;

    /// Send a WebSocket ping frame (no-op on non-WS transports).
    virtual bool sendPing() { return true; }

    /// Block until the next complete message arrives.
    /// Throws std::runtime_error on fatal transport error.
    /// Returns empty string when the connection is cleanly closed.
    virtual std::string receive() = 0;

    /// True while the underlying socket is open and usable.
    virtual bool isConnected() const = 0;

    /// Interrupt a pending receive() so the read thread can be joined.
    /// After stop() returns, receive() must return promptly (with empty
    /// or an error).  The transport may be reused after a subsequent
    /// connect() call.
    virtual void stop() = 0;
};

} // namespace market_data
} // namespace chronos
