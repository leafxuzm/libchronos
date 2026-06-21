#include "chronos/logging/zmq_bridge.hpp"
#include <zmq.h>
#include <cstring>

namespace chronos {
namespace logging {

namespace {

bool sendMessage(void* socket, const char* topic, const void* record, size_t record_size) {
    size_t msg_size = 4 + record_size;
    char buf[132];  // max of TICK(68), ORDER(132), FILL(132)
    std::memcpy(buf, topic, 4);
    std::memcpy(buf + 4, record, record_size);
    int rc = zmq_send(socket, buf, msg_size, ZMQ_DONTWAIT);
    return rc == static_cast<int>(msg_size);
}

}  // namespace

// ============================================================================
// Destructor
// ============================================================================

ZMQBridge::~ZMQBridge() {
    stop();
}

// ============================================================================
// Initialize / Stop
// ============================================================================

bool ZMQBridge::initialize(const ZMQConfig& config) {
    if (running_.load(std::memory_order_acquire)) return false;

    config_ = config;
    queue_ = std::make_unique<Queue>();

    zmq_context_ = zmq_ctx_new();
    if (!zmq_context_) return false;

    zmq_socket_ = zmq_socket(zmq_context_, ZMQ_PUB);
    if (!zmq_socket_) {
        zmq_ctx_destroy(zmq_context_);
        zmq_context_ = nullptr;
        return false;
    }

    // Configure socket
    int hwm = config_.send_hwm;
    zmq_setsockopt(zmq_socket_, ZMQ_SNDHWM, &hwm, sizeof(hwm));
    int linger = config_.linger_ms;
    zmq_setsockopt(zmq_socket_, ZMQ_LINGER, &linger, sizeof(linger));

    // Bind
    int rc = zmq_bind(zmq_socket_, config_.bind_address.c_str());
    if (rc != 0) {
        zmq_close(zmq_socket_);
        zmq_ctx_destroy(zmq_context_);
        zmq_socket_ = nullptr;
        zmq_context_ = nullptr;
        return false;
    }

    running_.store(true, std::memory_order_release);
    pub_thread_ = std::thread(&ZMQBridge::run, this);
    return true;
}

void ZMQBridge::stop() {
    if (!running_.load(std::memory_order_acquire)) return;
    running_.store(false, std::memory_order_release);
    if (pub_thread_.joinable()) {
        pub_thread_.join();
    }
    if (zmq_socket_) {
        zmq_close(zmq_socket_);
        zmq_socket_ = nullptr;
    }
    if (zmq_context_) {
        zmq_ctx_destroy(zmq_context_);
        zmq_context_ = nullptr;
    }
    queue_.reset();
}

// ============================================================================
// Hot path
// ============================================================================

bool ZMQBridge::publishTick(const Tick& tick) {
    Event ev;
    ev.tag = EventTag::TICK;
    std::memcpy(ev.data, &tick, sizeof(Tick));
    if (!queue_->try_push(ev)) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

bool ZMQBridge::publishOrder(const OrderRequest& order) {
    Event ev;
    ev.tag = EventTag::ORDER;
    std::memcpy(ev.data, &order, sizeof(OrderRequest));
    if (!queue_->try_push(ev)) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

bool ZMQBridge::publishFill(const Fill& fill) {
    Event ev;
    ev.tag = EventTag::FILL;
    std::memcpy(ev.data, &fill, sizeof(Fill));
    if (!queue_->try_push(ev)) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

// ============================================================================
// Publisher thread
// ============================================================================

void ZMQBridge::run() {
    Event ev;
    while (running_.load(std::memory_order_acquire)) {
        while (queue_->try_pop(ev)) {
            switch (ev.tag) {
                case EventTag::TICK:
                    if (sendMessage(zmq_socket_, "TICK", ev.data, sizeof(Tick))) {
                        ticks_pub_.fetch_add(1, std::memory_order_relaxed);
                    }
                    break;
                case EventTag::ORDER:
                    if (sendMessage(zmq_socket_, "ORDR", ev.data, sizeof(OrderRequest))) {
                        orders_pub_.fetch_add(1, std::memory_order_relaxed);
                    }
                    break;
                case EventTag::FILL:
                    if (sendMessage(zmq_socket_, "FILL", ev.data, sizeof(Fill))) {
                        fills_pub_.fetch_add(1, std::memory_order_relaxed);
                    }
                    break;
            }
        }
        // Brief sleep to avoid busy-spinning when queue is empty
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
}

}  // namespace logging
}  // namespace chronos
