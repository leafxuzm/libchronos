#include <chronos/market_data/adapters/okx_adapter.hpp>
#include <nlohmann/json.hpp>
#include <simdjson.h>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <unordered_set>
#include <unordered_map>

namespace chronos {
namespace market_data {
namespace adapters {

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace asio = boost::asio;
namespace ssl = asio::ssl;

struct OkxAdapter::Impl {
    // --- Boost.Beast transport ---
    using tcp = boost::asio::ip::tcp;
    using SSLSocket = boost::asio::ssl::stream<tcp::socket>;
    using WSStream = boost::beast::websocket::stream<SSLSocket&>;

    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl_ctx{boost::asio::ssl::context::tls_client};
    std::unique_ptr<SSLSocket> ssl_socket;
    std::unique_ptr<WSStream> ws_stream;
    simdjson::dom::parser json_parser;

    // --- Connection ---
    std::string ws_url_, ws_host_, ws_path_, ws_port_;

    // --- Threads ---
    std::thread ws_thread_;
    std::thread heartbeat_thread_;
    std::atomic<bool> heartbeat_running_{false};
    std::chrono::seconds heartbeat_interval_{25};

    // --- Subscription ---
    std::mutex symbols_mutex_;
    std::unordered_set<std::string> subscribed_symbols_;
    std::unordered_map<std::string, uint32_t> symbol_to_id_;
    uint32_t next_symbol_id_{1};

    // --- Stats (pointer to adapter's statistics_.ticks_processed) ---
    uint64_t* ticks_processed_ = nullptr;

    // ========================================================================
    // Self-contained helpers (no base-class access needed)
    // ========================================================================

    void doDisconnect() {
        try {
            if (ws_stream && ws_stream->is_open()) {
                ws_stream->close(websocket::close_code::normal);
            }
        } catch (...) {}
        ws_stream.reset();
        ssl_socket.reset();
    }

    void sendText(const std::string& message) {
        if (!ws_stream || !ws_stream->is_open()) return;
        boost::system::error_code ec;
        ws_stream->write(asio::buffer(message), ec);
        if (ec) {
            spdlog::error("OKX send error: {}", ec.message());
        }
    }

    std::string normalizeSymbol(const std::string& symbol) {
        std::string n = symbol;
        std::transform(n.begin(), n.end(), n.begin(), ::toupper);
        std::replace(n.begin(), n.end(), '_', '-');
        return n;
    }

    // Hot-path lookup: no lock. subscribe() pre-populates symbol_to_id_ with
    // lock held; ws_thread starts after subscribe() → happens-before guarantees visibility.
    uint32_t getSymbolId(const std::string& symbol) {
        std::string n = normalizeSymbol(symbol);
        auto it = symbol_to_id_.find(n);
        if (it != symbol_to_id_.end()) return it->second;

        spdlog::error("OKX: tick for unregistered symbol '{}' — dropped", n);
        return 0;
    }

    void sendSubscription(const std::vector<std::string>& symbols) {
        nlohmann::json args = nlohmann::json::array();
        for (const auto& sym : symbols) {
            args.push_back({{"channel", "books5"}, {"instId", sym}});
            args.push_back({{"channel", "trades"}, {"instId", sym}});
        }
        nlohmann::json req = {{"op", "subscribe"}, {"args", args}};
        sendText(req.dump());
    }

    void sendUnsubscription(const std::vector<std::string>& symbols) {
        nlohmann::json args = nlohmann::json::array();
        for (const auto& sym : symbols) {
            args.push_back({{"channel", "books5"}, {"instId", sym}});
            args.push_back({{"channel", "trades"}, {"instId", sym}});
        }
        nlohmann::json req = {{"op", "unsubscribe"}, {"args", args}};
        sendText(req.dump());
    }

    void startHeartbeat() {
        heartbeat_running_ = true;
        heartbeat_thread_ = std::thread([this]() {
            while (heartbeat_running_.load()) {
                std::this_thread::sleep_for(heartbeat_interval_);
                if (heartbeat_running_.load() && ws_stream && ws_stream->is_open()) {
                    sendText("ping");
                }
            }
        });
    }

    void stopHeartbeat() {
        heartbeat_running_ = false;
        if (heartbeat_thread_.joinable()) heartbeat_thread_.join();
    }

    // ========================================================================
    // Message parsing (needs tick_queue + receive_ts passed in)
    // ========================================================================

    void parseMessage(const std::string& message,
                      utils::MPMCQueue<Tick, 65536>& tick_queue,
                      uint64_t receive_ts) {
        simdjson::dom::element doc = json_parser.parse(message);
        simdjson::dom::object obj = doc.get_object();

        // Event message: {"event":"subscribe",...} or {"event":"error",...}
        if (obj["event"].error() == simdjson::SUCCESS) {
            std::string_view event = obj["event"].get_string();
            if (event == "subscribe") {
                spdlog::debug("OKX subscription confirmed");
            } else if (event == "error") {
                std::string_view msg = obj["msg"].get_string();
                spdlog::warn("OKX error: {}", std::string(msg.data(), msg.size()));
            }
            return;
        }

        // Data push: {"arg":{"channel":"...","instId":"..."},"data":[...]}
        if (obj["arg"].error() == simdjson::SUCCESS) {
            simdjson::dom::object arg = obj["arg"].get_object();
            std::string_view channel = arg["channel"].get_string();
            std::string_view inst_sv = arg["instId"].get_string();
            std::string inst_id(inst_sv);

            if (channel == "books5") {
                for (auto elem : obj["data"].get_array()) {
                    parseBooks5(elem.get_object(), inst_id, tick_queue, receive_ts);
                }
            } else if (channel == "trades") {
                for (auto elem : obj["data"].get_array()) {
                    parseTrade(elem.get_object(), inst_id, tick_queue, receive_ts);
                }
            }
        }
    }

    void parseBooks5(simdjson::dom::object data, const std::string& inst_id,
                     utils::MPMCQueue<Tick, 65536>& queue,
                     uint64_t bulk_receive_ts) {
        try {
            uint32_t symbol_id = getSymbolId(inst_id);
            if (symbol_id == 0) return;

            uint64_t exchange_ts = bulk_receive_ts;

            if (data["ts"].error() == simdjson::SUCCESS) {
                std::string_view ts_sv = data["ts"].get_string();
                exchange_ts = std::stoull(std::string(ts_sv)) * 1000;
            }

            auto emit = [&](simdjson::dom::array arr, TickSide side) {
                for (auto level : arr) {
                    simdjson::dom::array pair = level.get_array();
                    Tick tick{};
                    tick.exchange_id = static_cast<uint32_t>(ExchangeId::OKX);
                    tick.exchange_timestamp_us = exchange_ts;
                    tick.receive_timestamp_us = bulk_receive_ts;
                    tick.symbol_id = symbol_id;
                    tick.price = parseDecimal(pair.at(0).get_string());
                    tick.quantity = parseDecimal(pair.at(1).get_string());
                    tick.side = side;
                    tick.flags = 0;
                    if (queue.try_push(tick)) ++*ticks_processed_;
                }
            };

            emit(data["bids"].get_array(), TickSide::BID);
            emit(data["asks"].get_array(), TickSide::ASK);
        } catch (const simdjson::simdjson_error& e) {
            spdlog::error("OKX parseBooks5: {}", e.what());
        }
    }

    void parseTrade(simdjson::dom::object data, const std::string& inst_id,
                    utils::MPMCQueue<Tick, 65536>& queue,
                    uint64_t bulk_receive_ts) {
        try {
            uint32_t symbol_id = getSymbolId(inst_id);
            if (symbol_id == 0) return;

            std::string_view side_str = data["side"].get_string();
            TickSide side = (side_str == "buy") ? TickSide::BID : TickSide::TRADE;

            Tick tick{};
            tick.exchange_id = static_cast<uint32_t>(ExchangeId::OKX);
            std::string_view ts_sv = data["ts"].get_string();
            tick.exchange_timestamp_us = std::stoull(std::string(ts_sv)) * 1000;
            tick.receive_timestamp_us = bulk_receive_ts;
            tick.symbol_id = symbol_id;
            tick.price = parseDecimal(data["px"].get_string());
            tick.quantity = parseDecimal(data["sz"].get_string());
            tick.side = side;
            tick.flags = 1;
            if (queue.try_push(tick)) ++*ticks_processed_;
        } catch (const simdjson::simdjson_error& e) {
            spdlog::error("OKX parseTrade: {}", e.what());
        }
    }
};

// ============================================================================
// OkxAdapter — public/protected interface
// ============================================================================

OkxAdapter::OkxAdapter() : pimpl_(std::make_unique<Impl>()) {}

OkxAdapter::~OkxAdapter() {
    stop();
}

bool OkxAdapter::initialize(const ExchangeConfig& config,
                            utils::MPMCQueue<Tick, 65536>& tick_queue) {
    config_ = config;
    tick_queue_ = &tick_queue;
    pimpl_->ticks_processed_ = &statistics_.ticks_processed;

    pimpl_->ws_url_ = config.websocket_url.empty() ?
                      "wss://ws.okx.com:8443/ws/v5/public" :
                      config.websocket_url;

    std::string url = pimpl_->ws_url_;
    if (url.starts_with("wss://")) url = url.substr(6);
    else if (url.starts_with("ws://")) url = url.substr(5);

    auto colon_pos = url.find(':');
    auto slash_pos = url.find('/');

    pimpl_->ws_host_ = url.substr(0, std::min(colon_pos, slash_pos));

    if (colon_pos != std::string::npos && colon_pos < slash_pos) {
        pimpl_->ws_port_ = url.substr(colon_pos + 1, slash_pos - colon_pos - 1);
    } else {
        pimpl_->ws_port_ = pimpl_->ws_url_.starts_with("wss") ? "443" : "80";
    }

    pimpl_->ws_path_ = (slash_pos != std::string::npos) ? url.substr(slash_pos) : "/ws/v5/public";

    spdlog::info("OkxAdapter initialized: host={} port={} path={}",
                 pimpl_->ws_host_, pimpl_->ws_port_, pimpl_->ws_path_);
    return true;
}

bool OkxAdapter::start() {
    if (running_.load()) {
        spdlog::warn("OkxAdapter already running");
        return true;
    }

    should_reconnect_ = true;
    setStatus(ConnectionStatus::CONNECTING);

    if (!doConnect()) {
        return false;
    }

    onConnected();

    running_ = true;

    pimpl_->ws_thread_ = std::thread([this]() {
        try {
            readLoop();
        } catch (const std::exception& e) {
            spdlog::error("OKX read loop error: {}", e.what());
        }
    });

    return true;
}

void OkxAdapter::stop() {
    if (!running_.load()) return;

    running_ = false;
    should_reconnect_ = false;

    pimpl_->stopHeartbeat();
    pimpl_->ioc.stop();

    if (pimpl_->ws_thread_.joinable()) pimpl_->ws_thread_.join();
    pimpl_->doDisconnect();

    if (reconnection_thread_.joinable()) reconnection_thread_.join();

    setStatus(ConnectionStatus::DISCONNECTED);
    spdlog::info("OkxAdapter stopped");
}

bool OkxAdapter::subscribe(const std::string& symbol) {
    std::string normalized = pimpl_->normalizeSymbol(symbol);

    {
        std::lock_guard<std::mutex> lock(pimpl_->symbols_mutex_);
        if (pimpl_->subscribed_symbols_.find(normalized) != pimpl_->subscribed_symbols_.end()) {
            return true;
        }
        pimpl_->symbol_to_id_[normalized] = pimpl_->next_symbol_id_++;
    }

    std::vector<std::string> symbols = {normalized};

    try {
        pimpl_->sendSubscription(symbols);
        std::lock_guard<std::mutex> lock(pimpl_->symbols_mutex_);
        pimpl_->subscribed_symbols_.insert(normalized);
        spdlog::info("OKX subscribed to: {}", normalized);
        return true;
    } catch (const std::exception& e) {
        spdlog::error("OKX subscribe failed {}: {}", normalized, e.what());
        return false;
    }
}

bool OkxAdapter::unsubscribe(const std::string& symbol) {
    std::string normalized = pimpl_->normalizeSymbol(symbol);

    {
        std::lock_guard<std::mutex> lock(pimpl_->symbols_mutex_);
        if (pimpl_->subscribed_symbols_.find(normalized) == pimpl_->subscribed_symbols_.end()) {
            return true;
        }
    }

    std::vector<std::string> symbols = {normalized};

    try {
        pimpl_->sendUnsubscription(symbols);
        std::lock_guard<std::mutex> lock(pimpl_->symbols_mutex_);
        pimpl_->subscribed_symbols_.erase(normalized);
        spdlog::info("OKX unsubscribed from: {}", normalized);
        return true;
    } catch (const std::exception& e) {
        spdlog::error("OKX unsubscribe failed {}: {}", normalized, e.what());
        return false;
    }
}

// --- Connection management (needs base-class protected members) ---

bool OkxAdapter::doConnect() {
    try {
        bool use_proxy = (config_.proxy_port > 0 && !config_.proxy_host.empty());

        if (use_proxy) {
            Impl::tcp::resolver resolver(pimpl_->ioc);
            auto proxy_results = resolver.resolve(config_.proxy_host,
                                                   std::to_string(config_.proxy_port));

            Impl::tcp::socket proxy_sock(pimpl_->ioc);
            asio::connect(proxy_sock, proxy_results);

            std::string connect_req =
                "CONNECT " + pimpl_->ws_host_ + ":" + pimpl_->ws_port_ + " HTTP/1.1\r\n"
                "Host: " + pimpl_->ws_host_ + ":" + pimpl_->ws_port_ + "\r\n\r\n";
            asio::write(proxy_sock, asio::buffer(connect_req));

            boost::asio::streambuf response_buf;
            asio::read_until(proxy_sock, response_buf, "\r\n\r\n");
            std::string response_str(
                boost::asio::buffers_begin(response_buf.data()),
                boost::asio::buffers_begin(response_buf.data()) + response_buf.size());

            if (response_str.find("200") == std::string::npos) {
                throw std::runtime_error("Proxy CONNECT failed: " + response_str);
            }
            spdlog::info("OKX proxy tunnel via {}:{}",
                         config_.proxy_host, config_.proxy_port);

            pimpl_->ssl_socket = std::make_unique<Impl::SSLSocket>(
                std::move(proxy_sock), pimpl_->ssl_ctx);
        } else {
            Impl::tcp::resolver resolver(pimpl_->ioc);
            auto results = resolver.resolve(pimpl_->ws_host_, pimpl_->ws_port_);
            pimpl_->ssl_socket = std::make_unique<Impl::SSLSocket>(pimpl_->ioc, pimpl_->ssl_ctx);
            asio::connect(pimpl_->ssl_socket->lowest_layer(), results);
        }

        if (!SSL_set_tlsext_host_name(pimpl_->ssl_socket->native_handle(),
                                       pimpl_->ws_host_.c_str())) {
            throw std::runtime_error("SSL_set_tlsext_host_name failed");
        }

        pimpl_->ssl_socket->handshake(ssl::stream_base::client);

        pimpl_->ws_stream = std::make_unique<Impl::WSStream>(*pimpl_->ssl_socket);
        pimpl_->ws_stream->handshake(pimpl_->ws_host_, pimpl_->ws_path_);

        spdlog::info("OKX WebSocket connected to {}:{}{}",
                     pimpl_->ws_host_, pimpl_->ws_port_, pimpl_->ws_path_);
        return true;
    } catch (const std::exception& e) {
        spdlog::error("OKX connection failed: {}", e.what());
        onError(std::string("Connection failed: ") + e.what());
        pimpl_->doDisconnect();
        return false;
    }
}

void OkxAdapter::readLoop() {
    beast::flat_buffer buffer;

    while (running_.load()) {
        buffer.clear();
        boost::system::error_code ec;
        pimpl_->ws_stream->read(buffer, ec);

        if (ec == boost::asio::error::operation_aborted) break;
        if (ec == websocket::error::closed) break;
        if (ec) {
            spdlog::error("OKX read error: {}", ec.message());
            break;
        }

        std::string msg = beast::buffers_to_string(buffer.data());
        onMessage(msg);
    }

    if (running_.load()) {
        onDisconnected();
    }
}

// --- Protected overrides ---

void OkxAdapter::onConnected() {
    setStatus(ConnectionStatus::CONNECTED);
    pimpl_->startHeartbeat();

    // Resubscribe after reconnection
    std::vector<std::string> symbols;
    {
        std::lock_guard<std::mutex> lock(pimpl_->symbols_mutex_);
        if (!pimpl_->subscribed_symbols_.empty()) {
            for (const auto& s : pimpl_->subscribed_symbols_) {
                symbols.push_back(s);
            }
        }
    }
    if (!symbols.empty()) {
        pimpl_->sendSubscription(symbols);
    }

    spdlog::info("OkxAdapter connected");
}

void OkxAdapter::onDisconnected() {
    setStatus(ConnectionStatus::DISCONNECTED);
    pimpl_->stopHeartbeat();

    if (should_reconnect_.load()) {
        startReconnectionTimer();
    }

    spdlog::warn("OkxAdapter disconnected");
}

void OkxAdapter::onMessage(const std::string& message) {
    statistics_.messages_received++;

    // OKX text heartbeat
    if (message == "ping") {
        pimpl_->sendText("pong");
        return;
    }
    if (message == "pong") {
        return;
    }

    try {
        uint64_t ts = captureReceiveTimestamp();
        pimpl_->parseMessage(message, *tick_queue_, ts);
    } catch (const simdjson::simdjson_error& e) {
        statistics_.errors_count++;
        spdlog::error("OKX parse error: {}", e.what());
    }
}

void OkxAdapter::onError(const std::string& error) {
    notifyError(error);
}

void OkxAdapter::requestSnapshot() {}

} // namespace adapters
} // namespace market_data
} // namespace chronos
