#pragma once

#include <chronos/io/protocols/protocol.hpp>
#include <simdjson.h>

namespace chronos {
namespace market_data {

/// Binance JSON wire-protocol parser.
///
/// Handles two message formats:
///   - Combined stream (spot): {"stream":"btcusdt@depth","data":{...}}
///   - Raw event (futures):     {"e":"depthUpdate","E":...,"s":"BTCUSDT",...}
///
/// Heartbeat uses WebSocket ping frames (not text), so usesWsPing() = true.
class BinanceJsonProtocol : public Protocol {
public:
    BinanceJsonProtocol();
    ~BinanceJsonProtocol() override;

    size_t parse(const std::string& msg, uint64_t receive_ts,
                 std::function<void(Tick&&)> onTick,
                 std::function<uint32_t(const std::string&)> resolveSymbolId) override;

    std::string subscribeRequest(const std::vector<std::string>& symbols) override;
    std::string unsubscribeRequest(const std::vector<std::string>& symbols) override;

    bool isPingMessage(const std::string&) const override { return false; }
    bool isPongMessage(const std::string&) const override { return false; }
    std::string heartbeatPayload() const override { return {}; }
    bool usesWsPing() const override { return true; }

    std::string normalizeSymbol(const std::string& raw) const override;
    std::string defaultUrl() const override {
        return "wss://stream.binance.com:9443/ws";
    }

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace market_data
} // namespace chronos
