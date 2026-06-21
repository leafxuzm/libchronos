#include <chronos/market_data/adapters/binance_adapter.hpp>
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

static constexpr const char* DEPTH_STREAM_SUFFIX = "@depth";
static constexpr const char* TRADE_STREAM_SUFFIX = "@trade";
static constexpr const char* TICKER_STREAM_SUFFIX = "@ticker";

struct BinanceAdapter::Impl {
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
    std::chrono::seconds heartbeat_interval_{30};

    // --- Subscription ---
    std::mutex symbols_mutex_;
    std::unordered_set<std::string> subscribed_symbols_;
    std::unordered_map<std::string, uint32_t> symbol_to_id_;
    uint32_t next_symbol_id_{1};
    std::atomic<uint64_t> message_id_{1};

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
            spdlog::error("Binance send error: {}", ec.message());
        }
    }

    std::string normalizeSymbol(const std::string& symbol) {
        std::string n = symbol;
        std::transform(n.begin(), n.end(), n.begin(), ::tolower);
        return n;
    }

    // Hot-path lookup: no lock. subscribe() pre-populates symbol_to_id_ with
    // lock held; ws_thread starts after subscribe() → happens-before guarantees visibility.
    uint32_t getSymbolId(const std::string& symbol) {
        std::string n = normalizeSymbol(symbol);
        auto it = symbol_to_id_.find(n);
        if (it != symbol_to_id_.end()) return it->second;

        spdlog::error("Binance: tick for unregistered symbol '{}' — dropped", n);
        return 0;
    }

    void sendSubscription(const std::vector<std::string>& streams) {
        nlohmann::json req = {
            {"method", "SUBSCRIBE"},
            {"params", streams},
            {"id", message_id_++}
        };
        sendText(req.dump());
    }

    void sendUnsubscription(const std::vector<std::string>& streams) {
        nlohmann::json req = {
            {"method", "UNSUBSCRIBE"},
            {"params", streams},
            {"id", message_id_++}
        };
        sendText(req.dump());
    }

    void startHeartbeat() {
        heartbeat_running_ = true;
        heartbeat_thread_ = std::thread([this]() {
            while (heartbeat_running_.load()) {
                std::this_thread::sleep_for(heartbeat_interval_);
                if (heartbeat_running_.load() && ws_stream && ws_stream->is_open()) {
                    boost::system::error_code ec;
                    ws_stream->ping(websocket::ping_data{}, ec);
                    if (ec) {
                        spdlog::warn("Binance ping failed: {}", ec.message());
                    }
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

        // Combined stream (spot): {"stream":"...","data":{...}}
        if (obj["stream"].error() == simdjson::SUCCESS) {
            std::string_view sn = obj["stream"].get_string();
            simdjson::dom::object data = obj["data"].get_object();

            if (sn.find(DEPTH_STREAM_SUFFIX) != std::string_view::npos) {
                parseDepthUpdate(data, tick_queue, receive_ts);
            } else if (sn.find(TRADE_STREAM_SUFFIX) != std::string_view::npos) {
                parseTrade(data, tick_queue, receive_ts);
            } else if (sn.find(TICKER_STREAM_SUFFIX) != std::string_view::npos) {
                parse24hrTicker(data);
            }
            return;
        }

        // Raw stream (futures): {"e":"depthUpdate","E":...,"s":"...",...}
        if (obj["e"].error() == simdjson::SUCCESS) {
            std::string_view event = obj["e"].get_string();
            if (event == "depthUpdate") {
                parseDepthUpdate(obj, tick_queue, receive_ts);
            } else if (event == "aggTrade" || event == "trade") {
                parseTrade(obj, tick_queue, receive_ts);
            } else if (event == "24hrTicker") {
                parse24hrTicker(obj);
            }
            return;
        }

        // Subscription response
        if (obj["result"].error() == simdjson::SUCCESS) {
            uint64_t id = obj["id"].get_uint64();
            if (obj["result"].is_null()) {
                spdlog::debug("Binance subscription OK, id={}", id);
            } else {
                spdlog::warn("Binance subscription response, id={}", id);
            }
        }
    }

    void parseDepthUpdate(simdjson::dom::object data,
                          utils::MPMCQueue<Tick, 65536>& queue,
                          uint64_t bulk_receive_ts) {
        try {
            uint64_t exchange_ts = data["E"].get_uint64() * 1000;
            std::string_view symbol = data["s"].get_string();
            uint32_t symbol_id = getSymbolId(std::string(symbol));
            if (symbol_id == 0) return;

            auto emit = [&](simdjson::dom::array arr, TickSide side) {
                for (auto level : arr) {
                    simdjson::dom::array pair = level.get_array();
                    Tick tick{};
                    tick.exchange_id = static_cast<uint32_t>(ExchangeId::BINANCE);
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

            emit(data["b"].get_array(), TickSide::BID);
            emit(data["a"].get_array(), TickSide::ASK);
        } catch (const simdjson::simdjson_error& e) {
            spdlog::error("Binance parseDepthUpdate: {}", e.what());
        }
    }

    void parseTrade(simdjson::dom::object data,
                    utils::MPMCQueue<Tick, 65536>& queue,
                    uint64_t bulk_receive_ts) {
        try {
            std::string_view symbol = data["s"].get_string();
            uint32_t symbol_id = getSymbolId(std::string(symbol));
            if (symbol_id == 0) return;

            Tick tick{};
            tick.exchange_id = static_cast<uint32_t>(ExchangeId::BINANCE);
            tick.symbol_id = symbol_id;
            tick.price = parseDecimal(data["p"].get_string());
            tick.quantity = parseDecimal(data["q"].get_string());
            tick.exchange_timestamp_us = data["T"].get_uint64() * 1000;
            tick.receive_timestamp_us = bulk_receive_ts;
            tick.side = data["m"].get_bool() ? TickSide::TRADE : TickSide::BID;
            tick.flags = 1;
            if (queue.try_push(tick)) ++*ticks_processed_;
        } catch (const simdjson::simdjson_error& e) {
            spdlog::error("Binance parseTrade: {}", e.what());
        }
    }

    void parse24hrTicker(simdjson::dom::object) {}
};

// ============================================================================
// BinanceAdapter — public/protected interface
// ============================================================================

BinanceAdapter::BinanceAdapter() : pimpl_(std::make_unique<Impl>()) {}

BinanceAdapter::~BinanceAdapter() {
    stop();
}

bool BinanceAdapter::initialize(const ExchangeConfig& config,
                                utils::MPMCQueue<Tick, 65536>& tick_queue) {
    config_ = config;
    tick_queue_ = &tick_queue;
    pimpl_->ticks_processed_ = &statistics_.ticks_processed;

    pimpl_->ws_url_ = config.websocket_url.empty() ?
                      "wss://stream.binance.com:9443/ws" :
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

    pimpl_->ws_path_ = (slash_pos != std::string::npos) ? url.substr(slash_pos) : "/ws";

    // Pre-register symbols from combined stream URLs (e.g. .../stream?streams=btcusdt@depth/...)
    // so that the hot-path getSymbolId() only does lock-free lookups.
    if (pimpl_->ws_path_.find("streams=") != std::string::npos) {
        std::string path = pimpl_->ws_path_;
        auto qm = path.find('?');
        if (qm != std::string::npos) {
            std::string query = path.substr(qm + 1);
            auto streams_pos = query.find("streams=");
            if (streams_pos != std::string::npos) {
                std::string streams = query.substr(streams_pos + 8);  // past "streams="
                size_t pos = 0;
                while (pos < streams.size()) {
                    auto next = streams.find('/', pos);
                    std::string stream = streams.substr(pos, next - pos);
                    auto at = stream.find('@');
                    if (at != std::string::npos) {
                        std::string sym = stream.substr(0, at);
                        std::transform(sym.begin(), sym.end(), sym.begin(), ::tolower);
                        if (pimpl_->symbol_to_id_.find(sym) == pimpl_->symbol_to_id_.end()) {
                            pimpl_->symbol_to_id_[sym] = pimpl_->next_symbol_id_++;
                        }
                    }
                    if (next == std::string::npos) break;
                    pos = next + 1;
                }
            }
        }
    }

    spdlog::info("BinanceAdapter initialized: host={} port={} path={} ({} symbols pre-registered)",
                 pimpl_->ws_host_, pimpl_->ws_port_, pimpl_->ws_path_,
                 pimpl_->symbol_to_id_.size());
    return true;
}

bool BinanceAdapter::start() {
    if (running_.load()) {
        spdlog::warn("BinanceAdapter already running");
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
            spdlog::error("Binance read loop error: {}", e.what());
        }
    });

    return true;
}

void BinanceAdapter::stop() {
    if (!running_.load()) return;

    running_ = false;
    should_reconnect_ = false;

    pimpl_->stopHeartbeat();
    pimpl_->ioc.stop();

    if (pimpl_->ws_thread_.joinable()) pimpl_->ws_thread_.join();
    pimpl_->doDisconnect();

    if (reconnection_thread_.joinable()) reconnection_thread_.join();

    setStatus(ConnectionStatus::DISCONNECTED);
    spdlog::info("BinanceAdapter stopped");
}

bool BinanceAdapter::subscribe(const std::string& symbol) {
    std::string normalized = pimpl_->normalizeSymbol(symbol);

    {
        std::lock_guard<std::mutex> lock(pimpl_->symbols_mutex_);
        if (pimpl_->subscribed_symbols_.find(normalized) != pimpl_->subscribed_symbols_.end()) {
            return true;
        }
        pimpl_->symbol_to_id_[normalized] = pimpl_->next_symbol_id_++;
    }

    std::vector<std::string> streams = {
        normalized + DEPTH_STREAM_SUFFIX,
        normalized + TRADE_STREAM_SUFFIX,
        normalized + TICKER_STREAM_SUFFIX
    };

    try {
        pimpl_->sendSubscription(streams);
        std::lock_guard<std::mutex> lock(pimpl_->symbols_mutex_);
        pimpl_->subscribed_symbols_.insert(normalized);
        spdlog::info("Binance subscribed to: {}", normalized);
        return true;
    } catch (const std::exception& e) {
        spdlog::error("Binance subscribe failed {}: {}", normalized, e.what());
        return false;
    }
}

bool BinanceAdapter::unsubscribe(const std::string& symbol) {
    std::string normalized = pimpl_->normalizeSymbol(symbol);

    {
        std::lock_guard<std::mutex> lock(pimpl_->symbols_mutex_);
        if (pimpl_->subscribed_symbols_.find(normalized) == pimpl_->subscribed_symbols_.end()) {
            return true;
        }
    }

    std::vector<std::string> streams = {
        normalized + DEPTH_STREAM_SUFFIX,
        normalized + TRADE_STREAM_SUFFIX,
        normalized + TICKER_STREAM_SUFFIX
    };

    try {
        pimpl_->sendUnsubscription(streams);
        std::lock_guard<std::mutex> lock(pimpl_->symbols_mutex_);
        pimpl_->subscribed_symbols_.erase(normalized);
        spdlog::info("Binance unsubscribed from: {}", normalized);
        return true;
    } catch (const std::exception& e) {
        spdlog::error("Binance unsubscribe failed {}: {}", normalized, e.what());
        return false;
    }
}

// --- Connection management (needs base-class protected members) ---

bool BinanceAdapter::doConnect() {
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
            spdlog::info("Binance proxy tunnel via {}:{}",
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

        spdlog::info("Binance WebSocket connected to {}:{}{}",
                     pimpl_->ws_host_, pimpl_->ws_port_, pimpl_->ws_path_);
        return true;
    } catch (const std::exception& e) {
        spdlog::error("Binance connection failed: {}", e.what());
        onError(std::string("Connection failed: ") + e.what());
        pimpl_->doDisconnect();
        return false;
    }
}

void BinanceAdapter::readLoop() {
    beast::flat_buffer buffer;

    while (running_.load()) {
        buffer.clear();
        boost::system::error_code ec;
        pimpl_->ws_stream->read(buffer, ec);

        if (ec == boost::asio::error::operation_aborted) break;
        if (ec == websocket::error::closed) break;
        if (ec) {
            spdlog::error("Binance read error: {}", ec.message());
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

void BinanceAdapter::onConnected() {
    setStatus(ConnectionStatus::CONNECTED);
    pimpl_->startHeartbeat();

    // Resubscribe after reconnection
    std::vector<std::string> streams;
    {
        std::lock_guard<std::mutex> lock(pimpl_->symbols_mutex_);
        if (!pimpl_->subscribed_symbols_.empty()) {
            for (const auto& s : pimpl_->subscribed_symbols_) {
                streams.push_back(s + DEPTH_STREAM_SUFFIX);
                streams.push_back(s + TRADE_STREAM_SUFFIX);
                streams.push_back(s + TICKER_STREAM_SUFFIX);
            }
        }
    }
    if (!streams.empty()) {
        pimpl_->sendSubscription(streams);
    }

    spdlog::info("BinanceAdapter connected");
}

void BinanceAdapter::onDisconnected() {
    setStatus(ConnectionStatus::DISCONNECTED);
    pimpl_->stopHeartbeat();

    if (should_reconnect_.load()) {
        startReconnectionTimer();
    }

    spdlog::warn("BinanceAdapter disconnected");
}

void BinanceAdapter::onMessage(const std::string& message) {
    statistics_.messages_received++;

    try {
        uint64_t ts = captureReceiveTimestamp();
        pimpl_->parseMessage(message, *tick_queue_, ts);
    } catch (const simdjson::simdjson_error& e) {
        statistics_.errors_count++;
        spdlog::error("Binance parse error: {}", e.what());
    }
}

void BinanceAdapter::onError(const std::string& error) {
    notifyError(error);
}

void BinanceAdapter::requestSnapshot() {}

} // namespace adapters
} // namespace market_data
} // namespace chronos
