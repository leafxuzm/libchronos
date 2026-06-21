#include <chronos/market_data/adapters/thin_mux_adapter.hpp>
#include <chronos/market_data/adapter_concept.hpp>
#include <chronos/io/transports/transport.hpp>
#include <chronos/io/protocols/protocol.hpp>
#include <spdlog/spdlog.h>
#include <random>
#include <mutex>
#include <unordered_set>
#include <unordered_map>
#include <thread>

namespace chronos {
namespace market_data {

static_assert(ExchangeAdapter<ThinMuxAdapter>);

// ============================================================================
// Pimpl — all state and logic hidden from the header
// ============================================================================

struct ThinMuxAdapter::Impl {
    // --- Owned layers ---
    std::unique_ptr<Transport> transport;
    std::unique_ptr<Protocol>  protocol;
    ExchangeConfig             config;

    // --- Tick queue ---
    utils::MPMCQueue<Tick, 65536>* tick_queue = nullptr;

    // --- Status & callbacks ---
    std::atomic<ConnectionStatus> status{ConnectionStatus::DISCONNECTED};
    using SCallback = ThinMuxAdapter::StatusCallback;
    using ECallback = ThinMuxAdapter::ErrorCallback;
    SCallback status_callback;
    ECallback error_callback;

    // --- Statistics ---
    GatewayStatistics stats;

    // --- Threading ---
    std::atomic<bool> running{false};
    std::atomic<bool> should_reconnect{true};
    std::thread       read_thread;
    std::thread       heartbeat_thread;
    std::atomic<bool> heartbeat_running{false};
    std::mutex        heartbeat_mutex;
    std::condition_variable heartbeat_cv;
    std::thread       reconnect_thread;
    std::atomic<int>  reconnect_attempts{0};
    std::chrono::seconds heartbeat_interval{30};

    // --- Subscription state (heap-allocated so Impl stays movable) ---
    //
    // All subscribe() calls happen-before start() (user rule: pre-plan symbols).
    // std::thread constructor synchronizes with the new thread, so writes
    // to symbol_to_id are visible in readLoop() without any lock or atomic.
    struct SubState {
        std::mutex mutex;                           // serializes subscribe/unsubscribe
        std::unordered_set<std::string> subscribed;
        std::unordered_map<std::string, uint32_t> symbol_to_id;
        uint32_t next_symbol_id = 1;
    };
    std::unique_ptr<SubState> sub = std::make_unique<SubState>();

    // --- Internal helpers ---

    void setStatus(ConnectionStatus s) {
        ConnectionStatus old = status.exchange(s);
        if (old != s && status_callback) status_callback(s);
        spdlog::info("Gateway status: {} -> {}", toString(old), toString(s));
    }

    void notifyError(const std::string& msg) {
        stats.errors_count++;
        spdlog::error("Gateway error: {}", msg);
        if (error_callback) error_callback(msg);
    }

    bool pushTick(const Tick& tick) {
        if (!tick_queue) return false;
        if (tick_queue->try_push(tick)) {
            stats.ticks_processed++;
            stats.last_message_time = std::chrono::steady_clock::now();
            return true;
        }
        spdlog::warn("Tick queue full, dropping tick sym_id={}", tick.symbol_id);
        return false;
    }

    static uint64_t captureReceiveTimestamp() {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()).count();
    }

    uint32_t resolveSymbolId(const std::string& sym) {
        std::string n = protocol ? protocol->normalizeSymbol(sym) : sym;
        // No lock — map writes happen-before read thread creation (see SubState comment)
        auto it = sub->symbol_to_id.find(n);
        if (it != sub->symbol_to_id.end()) return it->second;
        spdlog::error("Unregistered symbol '{}' — dropped", n);
        return 0;
    }

    void resubscribeAll() {
        std::vector<std::string> symbols;
        {
            std::lock_guard lock(sub->mutex);
            for (const auto& s : sub->subscribed) symbols.push_back(s);
        }
        if (!symbols.empty() && protocol) {
            std::string req = protocol->subscribeRequest(symbols);
            if (transport) transport->send(req);
        }
    }

    void readLoop() {
        while (running.load() && transport && transport->isConnected()) {
            std::string msg;
            try {
                msg = transport->receive();
            } catch (const std::exception& e) {
                spdlog::error("Transport read error: {}", e.what());
                break;
            }

            if (msg.empty()) break;

            if (protocol->isPingMessage(msg)) {
                transport->send(protocol->heartbeatPayload());
                continue;
            }
            if (protocol->isPongMessage(msg)) continue;

            uint64_t ts = captureReceiveTimestamp();
            stats.messages_received++;

            auto onTick = [this](Tick&& t) { pushTick(t); };
            auto resolveId = [this](const std::string& s) -> uint32_t {
                return resolveSymbolId(s);
            };

            protocol->parse(msg, ts, onTick, resolveId);
        }

        if (running.load() && should_reconnect.load()) {
            setStatus(ConnectionStatus::DISCONNECTED);
            startReconnectTimer();
            spdlog::warn("ThinMuxAdapter disconnected — reconnect scheduled");
        }
    }

    void heartbeatLoop() {
        std::unique_lock lock(heartbeat_mutex);
        while (heartbeat_running.load()) {
            // Wait for interval or until stop() signals
            heartbeat_cv.wait_for(lock, heartbeat_interval,
                                  [this] { return !heartbeat_running.load(); });
            if (!heartbeat_running.load()) break;
            if (!transport || !transport->isConnected()) continue;

            if (protocol->usesWsPing()) {
                transport->sendPing();
            } else {
                transport->send(protocol->heartbeatPayload());
            }
        }
    }

    void startReconnectTimer() {
        if (!should_reconnect.load()) return;
        if (reconnect_thread.joinable()) reconnect_thread.join();
        reconnect_thread = std::thread([this]() { handleReconnect(); });
    }

    void handleReconnect() {
        int attempts = reconnect_attempts.load();
        int base_ms = std::min(1000 * (1 << attempts), 60000);

        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> jitter(-base_ms / 4, base_ms / 4);
        int delay_ms = base_ms + jitter(gen);

        spdlog::info("Reconnect attempt {} in {}ms", attempts + 1, delay_ms);
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));

        if (!should_reconnect.load()) return;

        reconnect_attempts++;
        stats.reconnections++;
    }
};

// ============================================================================
// ThinMuxAdapter — thin delegation to pimpl_
// ============================================================================

ThinMuxAdapter::ThinMuxAdapter() : pimpl_(std::make_unique<Impl>()) {}

ThinMuxAdapter::ThinMuxAdapter(std::unique_ptr<Transport> transport,
                               std::unique_ptr<Protocol> protocol)
    : pimpl_(std::make_unique<Impl>()) {
    pimpl_->transport = std::move(transport);
    pimpl_->protocol  = std::move(protocol);
}

ThinMuxAdapter::~ThinMuxAdapter() {
    if (pimpl_) stop();
}

ThinMuxAdapter::ThinMuxAdapter(ThinMuxAdapter&&) noexcept = default;
ThinMuxAdapter& ThinMuxAdapter::operator=(ThinMuxAdapter&&) noexcept = default;

// --- ExchangeAdapter contract ---

bool ThinMuxAdapter::initialize(const ExchangeConfig& cfg,
                                utils::MPMCQueue<Tick, 65536>& queue) {
    auto& p = *pimpl_;
    p.config = cfg;
    p.tick_queue = &queue;
    p.heartbeat_interval = std::chrono::seconds(
        cfg.heartbeat_interval_ms > 0 ? cfg.heartbeat_interval_ms / 1000 : 30);
    spdlog::info("ThinMuxAdapter initialized ({}:{})", cfg.name, cfg.websocket_url);
    return true;
}

bool ThinMuxAdapter::start() {
    auto& p = *pimpl_;
    if (p.running.load()) {
        spdlog::warn("ThinMuxAdapter already running");
        return true;
    }

    p.should_reconnect = true;
    p.setStatus(ConnectionStatus::CONNECTING);

    if (!p.transport) {
        spdlog::error("ThinMuxAdapter: no transport set");
        p.setStatus(ConnectionStatus::ERROR);
        return false;
    }

    if (!p.transport->connect(p.config)) {
        p.setStatus(ConnectionStatus::ERROR);
        return false;
    }

    p.setStatus(ConnectionStatus::CONNECTED);
    p.running = true;

    p.read_thread = std::thread([&p]() { p.readLoop(); });

    p.heartbeat_running = true;
    p.heartbeat_thread = std::thread([&p]() { p.heartbeatLoop(); });

    p.resubscribeAll();

    spdlog::info("ThinMuxAdapter started");
    return true;
}

void ThinMuxAdapter::stop() {
    if (!pimpl_) return;
    auto& p = *pimpl_;
    if (!p.running.load()) return;

    p.running = false;
    p.should_reconnect = false;

    p.heartbeat_running = false;
    {
        // Lock heartbeat_mutex to ensure heartbeatLoop is either
        // (a) past its initial mutex acquisition — notify_one will wake it, or
        // (b) not yet started — it will see heartbeat_running==false and exit.
        std::lock_guard lk(p.heartbeat_mutex);
    }
    p.heartbeat_cv.notify_one();
    if (p.heartbeat_thread.joinable()) p.heartbeat_thread.join();

    if (p.transport) p.transport->stop();

    if (p.read_thread.joinable()) p.read_thread.join();
    if (p.transport) p.transport->disconnect();
    if (p.reconnect_thread.joinable()) p.reconnect_thread.join();

    p.setStatus(ConnectionStatus::DISCONNECTED);
    spdlog::info("ThinMuxAdapter stopped");
}

bool ThinMuxAdapter::subscribe(std::string_view symbol) {
    auto& p = *pimpl_;
    if (!p.protocol) return false;

    std::string norm = p.protocol->normalizeSymbol(std::string(symbol));

    {
        std::lock_guard lock(p.sub->mutex);
        if (p.sub->subscribed.find(norm) != p.sub->subscribed.end()) return true;

        p.sub->symbol_to_id[norm] = p.sub->next_symbol_id++;
        p.sub->subscribed.insert(norm);
    }

    std::vector<std::string> syms = {norm};
    std::string req = p.protocol->subscribeRequest(syms);
    if (p.transport) p.transport->send(req);

    spdlog::info("Subscribed: {}", norm);
    return true;
}

bool ThinMuxAdapter::unsubscribe(std::string_view symbol) {
    auto& p = *pimpl_;
    if (!p.protocol) return false;

    std::string norm = p.protocol->normalizeSymbol(std::string(symbol));

    {
        std::lock_guard lock(p.sub->mutex);
        if (p.sub->subscribed.find(norm) == p.sub->subscribed.end()) return true;
    }

    std::vector<std::string> syms = {norm};
    std::string req = p.protocol->unsubscribeRequest(syms);
    if (p.transport) p.transport->send(req);

    {
        std::lock_guard lock(p.sub->mutex);
        p.sub->subscribed.erase(norm);
    }

    spdlog::info("Unsubscribed: {}", norm);
    return true;
}

bool ThinMuxAdapter::isRunning() const      { return pimpl_->running.load(); }
ConnectionStatus ThinMuxAdapter::getStatus() const { return pimpl_->status.load(); }
GatewayStatistics ThinMuxAdapter::getStatistics() const { return pimpl_->stats; }

void ThinMuxAdapter::setStatusCallback(StatusCallback cb) {
    pimpl_->status_callback = std::move(cb);
}
void ThinMuxAdapter::setErrorCallback(ErrorCallback cb) {
    pimpl_->error_callback = std::move(cb);
}

} // namespace market_data
} // namespace chronos
