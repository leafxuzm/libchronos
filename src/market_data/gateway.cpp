#include <chronos/market_data/gateway.hpp>
#include <chronos/market_data/adapters/binance_adapter.hpp>
#include <chronos/market_data/adapters/okx_adapter.hpp>
#include <chronos/core/error.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <random>

namespace chronos {
namespace market_data {

void MarketDataGateway::setStatus(ConnectionStatus status) {
    ConnectionStatus old_status = status_.exchange(status);
    if (old_status != status && status_callback_) {
        status_callback_(status);
    }
    
    spdlog::info("MarketDataGateway status changed: {} -> {}", 
                 toString(old_status), toString(status));
}

void MarketDataGateway::notifyError(const std::string& error) {
    statistics_.errors_count++;
    
    spdlog::error("MarketDataGateway error: {}", error);
    
    if (error_callback_) {
        error_callback_(error);
    }
}

bool MarketDataGateway::pushTick(const Tick& tick) {
    if (!tick_queue_) {
        spdlog::error("Tick queue not initialized");
        return false;
    }
    
    // Try to push tick to queue (non-blocking)
    if (tick_queue_->try_push(tick)) {
        statistics_.ticks_processed++;
        statistics_.last_message_time = std::chrono::steady_clock::now();
        return true;
    } else {
        // Queue is full, log warning but don't block
        spdlog::warn("Tick queue full, dropping tick for symbol {}", tick.symbol_id);
        return false;
    }
}

void MarketDataGateway::startReconnectionTimer() {
    if (!should_reconnect_.load()) {
        return;
    }
    
    // Stop existing reconnection thread if running
    if (reconnection_thread_.joinable()) {
        reconnection_thread_.join();
    }
    
    reconnection_thread_ = std::thread([this]() {
        handleReconnection();
    });
}

void MarketDataGateway::handleReconnection() {
    int attempts = reconnection_attempts_.load();
    
    // Exponential backoff with jitter
    // Base delay: 1s, max delay: 60s
    int base_delay_ms = std::min(1000 * (1 << attempts), 60000);
    
    // Add jitter (±25%)
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> jitter(-base_delay_ms / 4, base_delay_ms / 4);
    int delay_ms = base_delay_ms + jitter(gen);
    
    spdlog::info("Reconnection attempt {} in {}ms", attempts + 1, delay_ms);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    
    if (!should_reconnect_.load()) {
        return;
    }
    
    reconnection_attempts_++;
    statistics_.reconnections++;
    
    // Try to reconnect
    if (start()) {
        spdlog::info("Reconnection successful after {} attempts", attempts + 1);
        reconnection_attempts_ = 0;
        
        // Request orderbook snapshot after successful reconnection
        try {
            requestSnapshot();
        } catch (const std::exception& e) {
            spdlog::warn("Failed to request snapshot after reconnection: {}", e.what());
        }
    } else {
        spdlog::warn("Reconnection attempt {} failed", attempts + 1);
        
        // Continue trying if we should reconnect
        if (should_reconnect_.load()) {
            startReconnectionTimer();
        }
    }
}

uint64_t MarketDataGateway::captureReceiveTimestamp() const {
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
}

// Factory function implementations
std::unique_ptr<MarketDataGateway> createGateway(const std::string& exchange_name) {
    if (exchange_name == "binance") {
        return std::make_unique<adapters::BinanceAdapter>();
    }
    if (exchange_name == "okx") {
        return std::make_unique<adapters::OkxAdapter>();
    }

    spdlog::error("Unsupported exchange: {}", exchange_name);
    return nullptr;
}

} // namespace market_data
} // namespace chronos