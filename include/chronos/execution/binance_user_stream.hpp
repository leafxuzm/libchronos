#pragma once

#include <chronos/core/config.hpp>
#include <chronos/core/types.hpp>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace chronos {

// fwd
namespace market_data { struct Transport; }

namespace execution {

class BinanceHttpClient;
class OrderGateway;

// ============================================================================
// BinanceUserStream — listens for execution reports on user data stream
// ============================================================================
//
// Creates a Binance user data stream (listenKey), then connects a WebSocket
// to receive real-time execution reports, account updates, etc.
//
// Lifecycle:
//   1. BinanceHttpClient::createListenKey() → listenKey
//   2. WebSocketTlsTransport::connect(wss://testnet.binance.vision/ws/<key>)
//   3. Read-loop parses executionReport → OrderGateway::injectFill/Ack/Reject
//   4. On stop: close listenKey, disconnect WS
//
// Thread safety: owns its own read thread. Callbacks are invoked from
// that thread — OrderGateway is designed to accept this.

class BinanceUserStream {
public:
    BinanceUserStream();
    ~BinanceUserStream();

    BinanceUserStream(const BinanceUserStream&) = delete;
    BinanceUserStream& operator=(const BinanceUserStream&) = delete;

    /// Start the user data stream. Blocks during listenKey creation and
    /// WebSocket connect. Spawns a read thread on success.
    /// @return true if listenKey obtained and WS connected
    bool start(const ExchangeConfig& cfg,
               BinanceHttpClient* httpClient,
               OrderGateway* gateway);

    /// Stop the read thread, close listenKey, disconnect WS
    void stop();

    bool isRunning() const { return running_.load(std::memory_order_acquire); }

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
    std::atomic<bool> running_{false};
};

}  // namespace execution
}  // namespace chronos
