#pragma once

#include <chronos/io/protocols/protocol.hpp>
#include <simdjson.h>

namespace chronos {
namespace market_data {

/// OKX JSON wire-protocol parser.
///
/// Message format:
///   Event:  {"event":"subscribe",...} or {"event":"error",...}
///   Data:   {"arg":{"channel":"books5","instId":"BTC-USDT"},"data":[...]}
///
/// Heartbeat uses text "ping"/"pong", so usesWsPing() = false.
class OKXJsonProtocol : public Protocol {
public:
    OKXJsonProtocol();
    ~OKXJsonProtocol() override;

    size_t parse(const std::string& msg, uint64_t receive_ts,
                 std::function<void(Tick&&)> onTick,
                 std::function<uint32_t(const std::string&)> resolveSymbolId) override;

    std::string subscribeRequest(const std::vector<std::string>& symbols) override;
    std::string unsubscribeRequest(const std::vector<std::string>& symbols) override;

    bool isPingMessage(const std::string& msg) const override { return msg == "ping"; }
    bool isPongMessage(const std::string& msg) const override { return msg == "pong"; }
    std::string heartbeatPayload() const override { return "ping"; }
    bool usesWsPing() const override { return false; }

    std::string normalizeSymbol(const std::string& raw) const override;
    std::string defaultUrl() const override {
        return "wss://ws.okx.com:8443/ws/v5/public";
    }

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace market_data
} // namespace chronos
