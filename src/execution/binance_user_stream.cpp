#include <chronos/execution/binance_user_stream.hpp>
#include <chronos/execution/binance_http_client.hpp>
#include <chronos/execution/order_gateway.hpp>
#include <chronos/io/transports/websocket_tls_transport.hpp>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace chronos {
namespace execution {

struct BinanceUserStream::Impl {
    std::unique_ptr<market_data::Transport> transport;
    BinanceHttpClient* httpClient = nullptr;
    OrderGateway* gateway = nullptr;
    ExchangeConfig cfg;
    std::string listenKey;
    std::thread readThread;
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> threadExited{false};

    bool isRunning() const {
        return !stopRequested.load(std::memory_order_acquire)
            && gateway->isRunning();
    }

    bool createListenKey() {
        listenKey = httpClient->createListenKey(cfg);
        if (listenKey.empty()) {
            spdlog::error("Failed to create Binance listenKey");
            return false;
        }
        spdlog::info("Binance listenKey obtained: {}...",
                     listenKey.substr(0, 16));
        return true;
    }

    void parseExecutionReport(const nlohmann::json& data) {
        std::string eventType = data.value("e", "");
        if (eventType != "ORDER_TRADE_UPDATE") return;

        // Futures format: order data is nested under "o"
        const auto& o = data["o"];

        std::string status = o.value("X", "");      // order status
        std::string execType = o.value("x", "");    // execution type
        std::string symbol = o.value("s", "");
        std::string side = o.value("S", "BUY");
        uint64_t orderId = 0;
        if (o.contains("i")) orderId = o["i"].get<uint64_t>();

        spdlog::debug("ORDER_TRADE_UPDATE: symbol={} orderId={} status={} execType={}",
                      symbol, orderId, status, execType);

        // Resolve local order ID from clientOrderId first, fallback to exchange ID
        uint64_t localId = 0;
        if (o.contains("c")) {
            std::string cid = o["c"].get<std::string>();
            localId = gateway->resolveByClientOrderId(cid);
        }
        if (localId == 0) localId = orderId;  // fallback to exchange ID

        if (status == "NEW" || status == "ACCEPTED") {
            OrderAck ack{};
            ack.order_id = localId;
            ack.exchange_order_id = orderId;
            ack.status = OrderStatus::ACCEPTED;
            if (o.contains("T"))
                ack.timestamp_us = o["T"].get<uint64_t>() * 1000;
            gateway->injectOrderAck(ack);
            return;
        }

        if (status == "REJECTED" || status == "EXPIRED") {
            OrderReject reject{};
            reject.order_id = localId;
            if (o.contains("T"))
                reject.timestamp_us = o["T"].get<uint64_t>() * 1000;
            if (o.contains("r"))
                reject.reason = o["r"].get<std::string>();
            gateway->injectOrderReject(reject);
            return;
        }

        // FILLED / PARTIALLY_FILLED — send a fill for each trade
        if (execType == "TRADE" &&
            (status == "FILLED" || status == "PARTIALLY_FILLED")) {
            Fill fill{};
            fill.order_id = localId;
            fill.exchange_id = static_cast<uint32_t>(ExchangeId::BINANCE);
            fill.side = (side == "BUY") ? OrderSide::BUY : OrderSide::SELL;

            // last filled price
            if (o.contains("L")) {
                std::string priceStr = o["L"].get<std::string>();
                fill.fill_price = toDecimal(std::stod(priceStr));
            }
            // last filled quantity
            if (o.contains("l")) {
                std::string qtyStr = o["l"].get<std::string>();
                fill.fill_quantity = toDecimal(std::stod(qtyStr));
            }
            // fallback: use original quantity
            if (o.contains("q") && fill.fill_quantity == Decimal(0)) {
                std::string qtyStr = o["q"].get<std::string>();
                fill.fill_quantity = toDecimal(std::stod(qtyStr));
            }

            if (o.contains("T"))
                fill.exchange_timestamp_us = o["T"].get<uint64_t>() * 1000;
            fill.receive_timestamp_us = o.contains("E")
                ? o["E"].get<uint64_t>() * 1000 : 0;

            gateway->injectFill(fill);
        }
    }

    void readLoop() {
        auto receiveAndDispatch = [this]() {
            while (isRunning()) {
                std::string msg;
                try {
                    msg = transport->receive();
                } catch (const std::exception& e) {
                    spdlog::warn("User stream read error: {}", e.what());
                    return;
                }
                if (msg.empty()) return;

                try {
                    auto j = nlohmann::json::parse(msg);
                    parseExecutionReport(j);
                } catch (const nlohmann::json::parse_error& e) {
                    spdlog::debug("User stream: non-JSON ({}B): {}",
                                  msg.size(), msg.substr(0, 80));
                }
            }
        };

        receiveAndDispatch();

        // Reconnect loop — retry every 5s
        while (isRunning()) {
            spdlog::warn("User stream disconnected, reconnecting in 5s...");
            std::this_thread::sleep_for(std::chrono::seconds(5));
            if (!isRunning()) break;

            if (transport) transport->disconnect();

            if (!createListenKey()) continue;
            if (!isRunning()) break;

            ExchangeConfig wsCfg = cfg;
            wsCfg.websocket_url = cfg.user_stream_url + "/" + listenKey;

            transport = std::make_unique<market_data::WebSocketTlsTransport>();
            if (!isRunning()) break;
            if (transport->connect(wsCfg)) {
                spdlog::info("User stream reconnected");
                receiveAndDispatch();
            }
        }

        threadExited.store(true, std::memory_order_release);
    }
};

// ============================================================================
// Public interface
// ============================================================================

BinanceUserStream::BinanceUserStream()
    : pimpl_(std::make_unique<Impl>()) {}

BinanceUserStream::~BinanceUserStream() {
    stop();
}

bool BinanceUserStream::start(const ExchangeConfig& cfg,
                               BinanceHttpClient* httpClient,
                               OrderGateway* gateway) {
    auto& p = *pimpl_;
    p.cfg = cfg;
    p.httpClient = httpClient;
    p.gateway = gateway;

    if (!p.createListenKey()) return false;

    // Build WebSocket URL for user data stream
    ExchangeConfig wsCfg = cfg;
    wsCfg.websocket_url = cfg.user_stream_url + "/" + p.listenKey;

    p.transport = std::make_unique<market_data::WebSocketTlsTransport>();

    if (!p.transport->connect(wsCfg)) {
        spdlog::error("Failed to connect user data stream WS");
        return false;
    }

    running_.store(true, std::memory_order_release);
    p.readThread = std::thread([this]() { pimpl_->readLoop(); });

    spdlog::info("BinanceUserStream started (listenKey active)");
    return true;
}

void BinanceUserStream::stop() {
    if (!running_.load(std::memory_order_acquire)) return;
    running_.store(false, std::memory_order_release);

    auto& p = *pimpl_;
    p.stopRequested.store(true, std::memory_order_release);

    // Repeatedly interrupt the transport until the read thread exits.
    // During reconnection, the read thread may create a new transport;
    // we must keep interrupting whatever transport is currently active.
    while (!p.threadExited.load(std::memory_order_acquire)) {
        if (p.transport) p.transport->stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (p.readThread.joinable()) p.readThread.join();

    if (p.transport) p.transport->disconnect();

    if (!p.listenKey.empty() && p.httpClient) {
        p.httpClient->closeListenKey(p.cfg, p.listenKey);
    }

    spdlog::info("BinanceUserStream stopped");
}

}  // namespace execution
}  // namespace chronos
