#pragma once

#include "chronos/core/types.hpp"
#include "chronos/core/config.hpp"
#include "chronos/utils/mpmc_queue.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace chronos {
namespace execution {

class BinanceHttpClient;
class BinanceUserStream;

// ============================================================================
// Fill callback — pushed to StrategyEngine when exchange fills arrive
// ============================================================================

using FillCallback = std::function<void(const Fill&)>;

// ============================================================================
// Pending order tracking
// ============================================================================

struct PendingOrder {
    uint64_t order_id{0};
    uint64_t client_order_id{0};
    uint64_t submit_timestamp_us{0};
    uint32_t symbol_id{0};
    uint32_t exchange_id{0};
    uint32_t strategy_id{0};
    OrderSide side{OrderSide::BUY};
    OrderType type{OrderType::LIMIT};
    OrderStatus status{OrderStatus::PENDING};
    Decimal original_price{0};
    Decimal original_quantity{0};
    Decimal filled_quantity{0};
    uint64_t last_update_us{0};
};

// ============================================================================
// Statistics
// ============================================================================

struct OrderGatewayStats {
    uint64_t orders_received{0};
    uint64_t orders_sent{0};
    uint64_t orders_rejected{0};
    uint64_t orders_cancelled{0};
    uint64_t orders_modified{0};
    uint64_t fills_received{0};
    uint64_t fill_errors{0};
    uint64_t exchange_id_mismatch{0};
    uint64_t queue_empty_polls{0};
    uint64_t pending_order_count{0};
    uint64_t avg_send_latency_us{0};
};

// ============================================================================
// OrderGateway — per-exchange order submission and lifecycle management
// ============================================================================

class OrderGateway {
public:
    using OrderQueue = utils::MPMCQueue<OrderRequest, 1024>;

    /// @param exchange_id    Exchange identifier (must match what strategies set on orders)
    /// @param simulate_fills If true, auto-generate fills after sending orders (MVP mode)
    OrderGateway(const ExchangeConfig& config,
                 OrderQueue* order_queue,
                 FillCallback fill_callback,
                 uint32_t exchange_id = 0,
                 bool simulate_fills = true);

    ~OrderGateway();

    OrderGateway(const OrderGateway&) = delete;
    OrderGateway& operator=(const OrderGateway&) = delete;

    // --- Lifecycle ---

    void start();
    void stop();
    bool isRunning() const { return running_.load(std::memory_order_acquire); }

    /// Pin the gateway I/O thread to a specific CPU core (-1 = no affinity).
    void setCpuAffinity(int cpu) { cpu_affinity_ = cpu; }
    int cpuAffinity() const { return cpu_affinity_; }

    // --- Simulation injection (MVP) ---

    void injectFill(const Fill& fill);
    void injectOrderAck(const OrderAck& ack);
    void injectOrderReject(const OrderReject& reject);

    /// Resolve local order ID from clientOrderId (for user stream fill matching)
    uint64_t resolveByClientOrderId(const std::string& clientOrderId) const;

    // --- Real exchange execution (Binance testnet) ---

    void setHttpClient(BinanceHttpClient* client) { http_client_ = client; }
    void setUserStream(BinanceUserStream* stream) { user_stream_ = stream; }

    // --- Queries ---

    uint32_t exchangeId() const { return exchange_id_; }
    const std::string& exchangeName() const { return exchange_name_; }

    OrderGatewayStats getStats() const;
    void resetStats();
    size_t pendingCount() const;

private:
    void run();
    void processOrder(const OrderRequest& order);

    // Encoding stubs (MVP simulated mode)
    bool encodeOrder(const OrderRequest& order, std::string& wire_msg);
    bool encodeCancel(uint64_t order_id, std::string& wire_msg);
    bool encodeModify(uint64_t order_id, Decimal new_price, Decimal new_quantity,
                      std::string& wire_msg);

    // Real encoding (Binance JSON)
    bool encodeOrderJson(const OrderRequest& order,
                         std::string& symbol, std::string& side,
                         std::string& qty, std::string& price,
                         std::string& clientOrderId);
    std::string symbolIdToName(uint32_t symbol_id) const;

    bool simulateSend(const std::string& wire_msg, uint64_t send_time_us);
    bool realSendWithClientId(const OrderRequest& order,
                              const std::string& symbol,
                              const std::string& side,
                              const std::string& qty,
                              const std::string& price,
                              const std::string& clientOrderId);

    // Pending order tracking
    void trackPending(const OrderRequest& order, uint64_t submit_time_us);
    void updatePendingStatus(uint64_t order_id, OrderStatus status,
                             Decimal filled_qty = Decimal(0));
    void finalizeOrder(uint64_t order_id);
    const PendingOrder* findPending(uint64_t order_id) const;

    // --- Threading ---

    std::thread         io_thread_;
    std::atomic<bool>   running_{false};
    int                 cpu_affinity_{-1};

    // --- Configuration ---

    ExchangeConfig      config_;
    uint32_t            exchange_id_{0};
    std::string         exchange_name_;

    // --- Dependencies (no ownership) ---

    OrderQueue*         order_queue_;
    FillCallback        fill_callback_;
    bool                simulate_fills_{true};
    BinanceHttpClient*  http_client_{nullptr};
    BinanceUserStream*  user_stream_{nullptr};

    // --- Pending orders ---

    mutable std::mutex  pending_mutex_;
    std::unordered_map<uint64_t, PendingOrder> pending_orders_;
    std::unordered_map<uint64_t, uint64_t> exchange_to_local_;   // exch id → local id
    std::unordered_map<std::string, uint64_t> client_id_to_local_;  // clientOrderId → local id

    // --- Statistics ---

    mutable std::mutex  stats_mutex_;
    OrderGatewayStats   stats_;

    // Latency EMA: (avg * 7 + new) / 8
    std::atomic<uint64_t> avg_send_latency_us_{0};
};

}  // namespace execution
}  // namespace chronos
