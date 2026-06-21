#pragma once

#include <chronos/core/config.hpp>
#include <chronos/core/types.hpp>
#include <memory>
#include <string>

namespace chronos {
namespace execution {

// ============================================================================
// BinanceHttpClient — synchronous REST client for Binance testnet
// ============================================================================
//
// All endpoints target testnet.binance.vision. The client supports
// HTTP CONNECT proxy tunnelling (reuses cfg.proxy_host/proxy_port).
//
// Thread safety: not thread-safe. Serialize calls from a single I/O thread.

class BinanceHttpClient {
public:
    BinanceHttpClient();
    ~BinanceHttpClient();

    BinanceHttpClient(const BinanceHttpClient&) = delete;
    BinanceHttpClient& operator=(const BinanceHttpClient&) = delete;

    // --- Order endpoints ---

    /// POST /api/v3/order — place a new limit order
    /// @param symbol  e.g. "BTCUSDT"
    /// @param side    "BUY" or "SELL"
    /// @param quantity  string-formatted decimal
    /// @param price     string-formatted decimal
    /// @param newClientOrderId  client-specified order ID (optional)
    /// @return JSON response from Binance, or empty string on failure
    std::string placeOrder(const ExchangeConfig& cfg,
                           const std::string& symbol,
                           const std::string& side,
                           const std::string& quantity,
                           const std::string& price,
                           const std::string& newClientOrderId = "");

    /// DELETE /api/v3/order — cancel an order
    /// @return true on success
    bool cancelOrder(const ExchangeConfig& cfg,
                     const std::string& symbol,
                     uint64_t orderId);

    // --- User data stream endpoints ---

    /// POST /api/v3/userDataStream — create a listenKey
    /// @return listenKey string, or empty on failure
    std::string createListenKey(const ExchangeConfig& cfg);

    /// PUT /api/v3/userDataStream — keep alive (call every 30 minutes)
    bool keepAliveListenKey(const ExchangeConfig& cfg, const std::string& listenKey);

    /// DELETE /api/v3/userDataStream — close the stream
    bool closeListenKey(const ExchangeConfig& cfg, const std::string& listenKey);

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;

    /// Low-level signed POST/PUT/DELETE
    std::string signedRequest(const ExchangeConfig& cfg,
                              const std::string& method,
                              const std::string& path,
                              const std::string& queryParams);
};

}  // namespace execution
}  // namespace chronos
