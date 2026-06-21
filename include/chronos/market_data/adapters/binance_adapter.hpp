#pragma once

#include <chronos/market_data/gateway.hpp>
#include <memory>

namespace chronos {
namespace market_data {
namespace adapters {

class BinanceAdapter : public MarketDataGateway {
public:
    BinanceAdapter();
    ~BinanceAdapter() override;

    // MarketDataGateway interface
    bool initialize(const ExchangeConfig& config,
                    utils::MPMCQueue<Tick, 65536>& tick_queue) override;
    bool start() override;
    void stop() override;
    bool subscribe(const std::string& symbol) override;
    bool unsubscribe(const std::string& symbol) override;

protected:
    void onConnected() override;
    void onDisconnected() override;
    void onMessage(const std::string& message) override;
    void onError(const std::string& error) override;
    void requestSnapshot() override;

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;

    // Connection management (needs base-class protected members)
    bool doConnect();
    void readLoop();
};

} // namespace adapters
} // namespace market_data
} // namespace chronos
