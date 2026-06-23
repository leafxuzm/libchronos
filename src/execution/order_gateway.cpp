#include "chronos/execution/order_gateway.hpp"
#include "chronos/utils/cpu_affinity.hpp"
#include <chronos/execution/binance_http_client.hpp>
#include <chronos/execution/binance_user_stream.hpp>
#include <spdlog/spdlog.h>
#include <chrono>
#include <functional>
#include <sstream>

namespace chronos {
namespace execution {

// ============================================================================
// Helper: current timestamp in microseconds
// ============================================================================

namespace {
uint64_t now_us() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}
}  // namespace

// ============================================================================
// Constructor / Destructor
// ============================================================================

OrderGateway::OrderGateway(const ExchangeConfig& config,
                           OrderQueue* order_queue,
                           FillCallback fill_callback,
                           uint32_t exchange_id,
                           bool simulate_fills)
    : config_(config)
    , exchange_id_(exchange_id)
    , exchange_name_(config.name)
    , order_queue_(order_queue)
    , fill_callback_(std::move(fill_callback))
    , simulate_fills_(simulate_fills)
{}

OrderGateway::~OrderGateway() {
    stop();
}

// ============================================================================
// Lifecycle
// ============================================================================

void OrderGateway::start() {
    if (running_.load(std::memory_order_acquire)) return;
    running_.store(true, std::memory_order_release);
    io_thread_ = std::thread(&OrderGateway::run, this);
}

void OrderGateway::stop() {
    if (!running_.load(std::memory_order_acquire)) return;
    running_.store(false, std::memory_order_release);
    if (io_thread_.joinable()) {
        io_thread_.join();
    }
    cancelAllPending();
}

// ============================================================================
// Main I/O loop
// ============================================================================

void OrderGateway::run() {
    if (cpu_affinity_ >= 0) {
        utils::setCpuAffinity(cpu_affinity_);
    }

    while (running_.load(std::memory_order_acquire)) {
        OrderRequest order;
        bool did_work = false;

        while (order_queue_->try_pop(order)) {
            did_work = true;
            processOrder(order);
        }

        if (!did_work) {
            {
                std::lock_guard<std::mutex> lk(stats_mutex_);
                stats_.queue_empty_polls++;
            }
            utils::cpuRelax();
        }
    }

    // Drain remaining queue items after stop signal
    OrderRequest order;
    while (order_queue_->try_pop(order)) {
        processOrder(order);
    }
}

// ============================================================================
// Order processing
// ============================================================================

void OrderGateway::processOrder(const OrderRequest& order) {
    {
        std::lock_guard<std::mutex> lk(stats_mutex_);
        stats_.orders_received++;
    }

    // Filter by exchange
    if (order.exchange_id != exchange_id_) {
        std::lock_guard<std::mutex> lk(stats_mutex_);
        stats_.exchange_id_mismatch++;
        return;
    }

    // Cancel: quantity == 0 signals cancellation
    if (order.quantity == Decimal(0)) {
        uint64_t cancel_id = order.order_id;
        uint64_t send_time = now_us();

        if (!simulate_fills_ && http_client_) {
            // Real path: DELETE to Binance
            std::string sym = symbolIdToName(order.symbol_id);
            if (!sym.empty()) {
                bool ok = http_client_->cancelOrder(config_, sym, cancel_id);
                if (ok) {
                    spdlog::info("Cancel order {} sent to Binance", cancel_id);
                }
            }
        } else {
            std::string wire_msg;
            if (!encodeCancel(cancel_id, wire_msg)) {
                std::lock_guard<std::mutex> lk(stats_mutex_);
                stats_.orders_rejected++;
                return;
            }
            simulateSend(wire_msg, send_time);
        }

        {
            std::lock_guard<std::mutex> lk(pending_mutex_);
            auto it = pending_orders_.find(cancel_id);
            if (it != pending_orders_.end()) {
                it->second.status = OrderStatus::CANCELLED;
                it->second.last_update_us = send_time;
                finalizeOrder(cancel_id);
            }
        }

        {
            std::lock_guard<std::mutex> lk(stats_mutex_);
            stats_.orders_cancelled++;
        }
        return;
    }

    // Check for modify: same order_id as existing pending order
    {
        std::lock_guard<std::mutex> lk(pending_mutex_);
        auto it = pending_orders_.find(order.order_id);
        if (it != pending_orders_.end()) {
            OrderStatus s = it->second.status;
            if (s == OrderStatus::PENDING || s == OrderStatus::ACCEPTED ||
                s == OrderStatus::PARTIALLY_FILLED) {

                std::string wire_msg;
                if (!encodeModify(order.order_id, order.price, order.quantity,
                                  wire_msg)) {
                    std::lock_guard<std::mutex> lk2(stats_mutex_);
                    stats_.orders_rejected++;
                    return;
                }

                uint64_t send_time = now_us();
                simulateSend(wire_msg, send_time);

                it->second.original_price = order.price;
                it->second.original_quantity = order.quantity;
                it->second.last_update_us = send_time;

                {
                    std::lock_guard<std::mutex> lk2(stats_mutex_);
                    stats_.orders_modified++;
                }
                return;
            }
        }
    }

    // New order
    {
        uint64_t send_time = now_us();

        if (!simulate_fills_ && http_client_) {
            // Real path: POST to Binance testnet
            // Track BEFORE REST call so user-stream fills can match by clientOrderId
            std::string symbol, side, qty, price, clientOrderId;
            if (!encodeOrderJson(order, symbol, side, qty, price, clientOrderId)) {
                std::lock_guard<std::mutex> lk(stats_mutex_);
                stats_.orders_rejected++;
                return;
            }
            {
                std::lock_guard<std::mutex> lk(pending_mutex_);
                client_id_to_local_[clientOrderId] = order.order_id;
            }
            trackPending(order, send_time);

            bool sent = realSendWithClientId(order, symbol, side, qty, price, clientOrderId);
            {
                std::lock_guard<std::mutex> lk(stats_mutex_);
                if (!sent) {
                    stats_.orders_rejected++;
                } else {
                    stats_.orders_sent++;
                }
            }
            if (!sent) return;
            // Fill/ack only from user stream ORDER_TRADE_UPDATE events
            return;
        } else {
            // Simulated path: stub encoding + auto-fill
            std::string wire_msg;
            if (!encodeOrder(order, wire_msg)) {
                std::lock_guard<std::mutex> lk(stats_mutex_);
                stats_.orders_rejected++;
                return;
            }

            simulateSend(wire_msg, send_time);
            trackPending(order, send_time);

            // Auto-simulated fill for MVP mode
            if (simulate_fills_ && fill_callback_) {
                Fill fill{};
                fill.order_id = order.order_id;
                fill.symbol_id = order.symbol_id;
                fill.fill_price = order.price;
                fill.fill_quantity = order.quantity;
                fill.side = order.side;
                fill.exchange_id = order.exchange_id;
                fill.strategy_id = order.strategy_id;
                fill.exchange_timestamp_us = send_time;
                fill.receive_timestamp_us = send_time;
                injectFill(fill);
            }
        }

        std::lock_guard<std::mutex> lk(stats_mutex_);
        stats_.orders_sent++;
    }
}

// ============================================================================
// Encoding stubs (MVP)
// ============================================================================

bool OrderGateway::encodeOrder(const OrderRequest& order, std::string& wire_msg) {
    wire_msg = "NEW|" + std::to_string(order.order_id) + "|"
             + std::to_string(order.symbol_id) + "|"
             + std::to_string(toDouble(order.price)) + "|"
             + std::to_string(toDouble(order.quantity)) + "|"
             + std::to_string(static_cast<int>(order.side)) + "|"
             + std::to_string(static_cast<int>(order.type));
    return true;
}

bool OrderGateway::encodeCancel(uint64_t order_id, std::string& wire_msg) {
    wire_msg = "CANCEL|" + std::to_string(order_id);
    return true;
}

bool OrderGateway::encodeModify(uint64_t order_id, Decimal new_price,
                                Decimal new_quantity, std::string& wire_msg) {
    wire_msg = "MODIFY|" + std::to_string(order_id) + "|"
             + std::to_string(toDouble(new_price)) + "|"
             + std::to_string(toDouble(new_quantity));
    return true;
}

bool OrderGateway::simulateSend(const std::string& /*wire_msg*/,
                                uint64_t send_time_us) {
    uint64_t elapsed = now_us() - send_time_us;
    uint64_t old_avg = avg_send_latency_us_.load(std::memory_order_relaxed);
    uint64_t new_avg = (old_avg == 0) ? elapsed : (old_avg * 7 + elapsed) / 8;
    avg_send_latency_us_.store(new_avg, std::memory_order_relaxed);
    return true;
}

bool OrderGateway::encodeOrderJson(const OrderRequest& order,
                                    std::string& symbol, std::string& side,
                                    std::string& qty, std::string& price,
                                    std::string& clientOrderId) {
    symbol = symbolIdToName(order.symbol_id);
    if (symbol.empty()) {
        spdlog::error("Cannot map symbol_id={} to name", order.symbol_id);
        return false;
    }
    side = (order.side == OrderSide::BUY) ? "BUY" : "SELL";
    qty = std::to_string(toDouble(order.quantity));
    price = std::to_string(toDouble(order.price));
    clientOrderId = std::to_string(order.order_id) + "_" + std::to_string(now_us());
    return true;
}

std::string OrderGateway::symbolIdToName(uint32_t symbol_id) const {
    // symbol_id is 1-based index into config_.symbols
    if (symbol_id == 0 || symbol_id > config_.symbols.size()) return {};
    return config_.symbols[symbol_id - 1];
}

bool OrderGateway::realSendWithClientId(const OrderRequest& order,
                                        const std::string& symbol,
                                        const std::string& side,
                                        const std::string& qty,
                                        const std::string& price,
                                        const std::string& clientOrderId) {
    if (!http_client_) {
        spdlog::error("realSend called but no http_client set");
        return false;
    }

    std::string resp = http_client_->placeOrder(
        config_, symbol, side, qty, price, clientOrderId);

    if (resp.empty()) {
        spdlog::warn("Order {} REST failed (symbol={} side={} qty={} price={})",
                     order.order_id, symbol, side, qty, price);
        return false;
    }

    try {
        auto j = nlohmann::json::parse(resp);
        OrderAck ack{};
        ack.order_id = order.order_id;
        ack.exchange_order_id = j.value("orderId", uint64_t(0));
        ack.status = OrderStatus::ACCEPTED;
        ack.timestamp_us = j.value("updateTime", uint64_t(0)) * 1000;
        injectOrderAck(ack);

        spdlog::info("Order {} sent to Binance: {} {} {}@{} (exchId={})",
                     order.order_id, symbol, side, qty, price,
                     ack.exchange_order_id);
    } catch (const std::exception& e) {
        spdlog::warn("Order {} REST parse failed: {}", order.order_id, e.what());
    }

    return true;
}

// ============================================================================
// Pending order tracking
// ============================================================================

void OrderGateway::trackPending(const OrderRequest& order,
                                uint64_t submit_time_us) {
    PendingOrder po;
    po.order_id          = order.order_id;
    po.client_order_id   = order.client_order_id;
    po.submit_timestamp_us = submit_time_us;
    po.symbol_id         = order.symbol_id;
    po.exchange_id       = order.exchange_id;
    po.strategy_id       = order.strategy_id;
    po.side              = order.side;
    po.type              = order.type;
    po.status            = OrderStatus::PENDING;
    po.original_price    = order.price;
    po.original_quantity = order.quantity;
    po.filled_quantity   = Decimal(0);
    po.last_update_us    = submit_time_us;

    std::lock_guard<std::mutex> lk(pending_mutex_);
    pending_orders_[order.order_id] = po;
}

void OrderGateway::updatePendingStatus(uint64_t order_id, OrderStatus status,
                                       Decimal filled_qty) {
    std::lock_guard<std::mutex> lk(pending_mutex_);
    auto it = pending_orders_.find(order_id);
    if (it == pending_orders_.end()) return;

    it->second.status = status;
    it->second.last_update_us = now_us();

    if (filled_qty != Decimal(0)) {
        it->second.filled_quantity += filled_qty;
    }

    if (status == OrderStatus::FILLED || status == OrderStatus::CANCELLED ||
        status == OrderStatus::REJECTED || status == OrderStatus::EXPIRED) {
        // Erase under lock — finalizeOrder called with lock already held
        pending_orders_.erase(it);
    }
}

void OrderGateway::finalizeOrder(uint64_t order_id) {
    // Caller must hold pending_mutex_
    pending_orders_.erase(order_id);
}

void OrderGateway::cancelAllPending() {
    std::vector<uint64_t> pending_ids;
    {
        std::lock_guard<std::mutex> lk(pending_mutex_);
        pending_ids.reserve(pending_orders_.size());
        for (const auto& kv : pending_orders_) {
            pending_ids.push_back(kv.first);
        }
    }

    for (uint64_t id : pending_ids) {
        // Real path: send cancel request to exchange
        if (!simulate_fills_ && http_client_) {
            std::string sym;
            {
                std::lock_guard<std::mutex> lk(pending_mutex_);
                auto it = pending_orders_.find(id);
                if (it == pending_orders_.end()) continue;
                sym = symbolIdToName(it->second.symbol_id);
            }
            if (!sym.empty()) {
                bool ok = http_client_->cancelOrder(config_, sym, id);
                if (ok) {
                    spdlog::info("Shutdown cancel: order {} on {}", id, sym);
                }
            }
        }

        // Erase from pending (both real and simulated paths)
        {
            std::lock_guard<std::mutex> lk(pending_mutex_);
            auto it = pending_orders_.find(id);
            if (it != pending_orders_.end()) {
                it->second.status = OrderStatus::CANCELLED;
                pending_orders_.erase(it);
            }
        }
        {
            std::lock_guard<std::mutex> lk(stats_mutex_);
            stats_.orders_cancelled++;
        }
    }

    if (!pending_ids.empty()) {
        spdlog::info("Shutdown: cancelled {} pending orders", pending_ids.size());
    }
}

const PendingOrder* OrderGateway::findPending(uint64_t order_id) const {
    std::lock_guard<std::mutex> lk(pending_mutex_);
    auto it = pending_orders_.find(order_id);
    if (it == pending_orders_.end()) return nullptr;
    return &it->second;
}

// ============================================================================
// Simulation injection (MVP)
// ============================================================================

void OrderGateway::injectFill(const Fill& fill) {
    // Populate strategy_id from pending order BEFORE invoking callback.
    // User-stream fills don't carry strategy_id — we must look it up.
    Fill enriched = fill;
    {
        std::lock_guard<std::mutex> lk(pending_mutex_);
        auto it = pending_orders_.find(fill.order_id);
        if (it == pending_orders_.end()) {
            auto ex = exchange_to_local_.find(fill.order_id);
            if (ex != exchange_to_local_.end()) {
                it = pending_orders_.find(ex->second);
            }
        }
        if (it == pending_orders_.end()) {
            std::lock_guard<std::mutex> lk2(stats_mutex_);
            stats_.fill_errors++;
            return;
        }

        enriched.strategy_id = it->second.strategy_id;
        enriched.symbol_id   = it->second.symbol_id;

        it->second.filled_quantity += fill.fill_quantity;
        it->second.last_update_us = now_us();

        if (it->second.filled_quantity >= it->second.original_quantity) {
            it->second.status = OrderStatus::FILLED;
            pending_orders_.erase(it);
        } else {
            it->second.status = OrderStatus::PARTIALLY_FILLED;
        }
    }

    // Invoke callback outside lock to prevent re-entrancy issues
    if (fill_callback_) {
        fill_callback_(enriched);
    }

    {
        std::lock_guard<std::mutex> lk(stats_mutex_);
        stats_.fills_received++;
    }
}

void OrderGateway::injectOrderAck(const OrderAck& ack) {
    std::lock_guard<std::mutex> lk(pending_mutex_);
    auto it = pending_orders_.find(ack.order_id);
    if (it != pending_orders_.end()) {
        it->second.status = OrderStatus::ACCEPTED;
        it->second.last_update_us = ack.timestamp_us;
        if (ack.exchange_order_id != 0) {
            exchange_to_local_[ack.exchange_order_id] = ack.order_id;
        }
    }
}

void OrderGateway::injectOrderReject(const OrderReject& reject) {
    {
        std::lock_guard<std::mutex> lk(pending_mutex_);
        auto it = pending_orders_.find(reject.order_id);
        if (it != pending_orders_.end()) {
            it->second.status = OrderStatus::REJECTED;
            it->second.last_update_us = reject.timestamp_us;
            pending_orders_.erase(it);
        }
    }
    {
        std::lock_guard<std::mutex> lk(stats_mutex_);
        stats_.orders_rejected++;
    }
}

uint64_t OrderGateway::resolveByClientOrderId(const std::string& clientOrderId) const {
    std::lock_guard<std::mutex> lk(pending_mutex_);
    auto it = client_id_to_local_.find(clientOrderId);
    if (it != client_id_to_local_.end()) return it->second;
    return 0;
}

// ============================================================================
// Statistics
// ============================================================================

OrderGatewayStats OrderGateway::getStats() const {
    OrderGatewayStats s;
    {
        std::lock_guard<std::mutex> lk(stats_mutex_);
        s = stats_;
    }
    {
        std::lock_guard<std::mutex> lk(pending_mutex_);
        s.pending_order_count = pending_orders_.size();
    }
    s.avg_send_latency_us = avg_send_latency_us_.load(std::memory_order_relaxed);
    return s;
}

void OrderGateway::resetStats() {
    std::lock_guard<std::mutex> lk(stats_mutex_);
    stats_ = OrderGatewayStats{};
    avg_send_latency_us_.store(0, std::memory_order_relaxed);
}

size_t OrderGateway::pendingCount() const {
    std::lock_guard<std::mutex> lk(pending_mutex_);
    return pending_orders_.size();
}

}  // namespace execution
}  // namespace chronos
