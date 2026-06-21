#pragma once

#include <chronos/core/types.hpp>
#include <chronos/core/config.hpp>
#include <chronos/utils/mpmc_queue.hpp>
#include <string>
#include <memory>
#include <functional>
#include <atomic>
#include <thread>
#include <chrono>

namespace chronos {
namespace market_data {

/**
 * @brief Base class for market data gateways
 * 
 * Provides a common interface for connecting to different exchanges
 * and receiving market data. Handles WebSocket connections, reconnection
 * logic, and data normalization to internal Tick format.
 */
class MarketDataGateway {
public:
    using StatusCallback = std::function<void(ConnectionStatus)>;
    using ErrorCallback = std::function<void(const std::string&)>;

    MarketDataGateway() = default;
    virtual ~MarketDataGateway() = default;

    // Non-copyable, non-movable
    MarketDataGateway(const MarketDataGateway&) = delete;
    MarketDataGateway& operator=(const MarketDataGateway&) = delete;

    virtual bool initialize(const ExchangeConfig& config, 
                          utils::MPMCQueue<Tick, 65536>& tick_queue) = 0;

    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual bool subscribe(const std::string& symbol) = 0;
    virtual bool unsubscribe(const std::string& symbol) = 0;
    ConnectionStatus getStatus() const { return status_.load(); }
    void setStatusCallback(StatusCallback callback) {
        status_callback_ = std::move(callback);
    }
    void setErrorCallback(ErrorCallback callback) {
        error_callback_ = std::move(callback);
    }

    struct Statistics {
        uint64_t messages_received = 0;
        uint64_t ticks_processed = 0;
        uint64_t errors_count = 0;
        uint64_t reconnections = 0;
        std::chrono::microseconds avg_latency{0};
        std::chrono::steady_clock::time_point last_message_time;
    };

    Statistics getStatistics() const { return statistics_; }

protected:
    virtual void onConnected() = 0;
    virtual void onDisconnected() = 0;
    virtual void onMessage(const std::string& message) = 0;
    virtual void onError(const std::string& error) = 0;


    void setStatus(ConnectionStatus status);
    void notifyError(const std::string& error);
    bool pushTick(const Tick& tick);
    
    void startReconnectionTimer();
    void handleReconnection();

    virtual void requestSnapshot() = 0;

    uint64_t captureReceiveTimestamp() const;

protected:
    // Configuration
    ExchangeConfig config_;
    
    // Tick queue reference
    utils::MPMCQueue<Tick, 65536>* tick_queue_ = nullptr;
    
    // Status and callbacks
    std::atomic<ConnectionStatus> status_{ConnectionStatus::DISCONNECTED};
    StatusCallback status_callback_;
    ErrorCallback error_callback_;
    
    // Reconnection logic
    std::atomic<bool> should_reconnect_{true};
    std::atomic<int> reconnection_attempts_{0};
    std::thread reconnection_thread_;
    
    // Statistics
    mutable Statistics statistics_;
    
    // Threading
    std::atomic<bool> running_{false};
    std::thread worker_thread_;
};

/**
 * @brief Factory function to create exchange-specific gateways
 */
std::unique_ptr<MarketDataGateway> createGateway(const std::string& exchange_name);

} // namespace market_data
} // namespace chronos