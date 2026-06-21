#pragma once

#include <chronos/io/transports/transport.hpp>
#include <memory>
#include <string>

namespace chronos {
namespace market_data {

/// WebSocket-over-TLS transport using Boost.Beast.
///
/// One instance per exchange connection.  Internally manages the
/// boost::asio::io_context, SSL context, TLS socket, and WebSocket stream.
/// Supports HTTP CONNECT proxy tunnelling when cfg.proxy_host is set.
///
/// Thread-safety: connect() / disconnect() / send() / receive() / stop()
/// are all called from the owning ThinMuxAdapter which guarantees at most
/// one thread in receive() at a time.
class WebSocketTlsTransport final : public Transport {
public:
    WebSocketTlsTransport();
    ~WebSocketTlsTransport() override;

    bool connect(const ExchangeConfig& cfg) override;
    void disconnect() override;
    bool send(const std::string& msg) override;
    bool sendPing() override;
    std::string receive() override;
    bool isConnected() const override;
    void stop() override;

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace market_data
} // namespace chronos
