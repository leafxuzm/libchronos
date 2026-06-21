#include <chronos/io/protocols/binance_json_protocol.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>

namespace chronos {
namespace market_data {

static constexpr const char* DEPTH_SUFFIX  = "@depth5@100ms";
static constexpr const char* TRADE_SUFFIX  = "@aggTrade";
static constexpr const char* TICKER_SUFFIX = "@bookTicker";

struct BinanceJsonProtocol::Impl {
    simdjson::dom::parser json_parser;

    void parseDepthUpdate(simdjson::dom::object data, uint64_t recv_ts,
                          std::function<void(Tick&&)> onTick,
                          std::function<uint32_t(const std::string&)> resolveSymbolId) {
        uint64_t exchange_ts = data["E"].get_uint64() * 1000;
        std::string_view symbol = data["s"].get_string();
        uint32_t sid = resolveSymbolId(std::string(symbol));
        if (sid == 0) return;

        auto emit = [&](simdjson::dom::array arr, TickSide side) {
            for (auto level : arr) {
                simdjson::dom::array pair = level.get_array();
                Tick tick{};
                tick.exchange_id = static_cast<uint32_t>(ExchangeId::BINANCE);
                tick.exchange_timestamp_us = exchange_ts;
                tick.receive_timestamp_us = recv_ts;
                tick.symbol_id = sid;
                tick.price = parseDecimal(pair.at(0).get_string());
                tick.quantity = parseDecimal(pair.at(1).get_string());
                tick.side = side;
                tick.flags = 0;
                onTick(std::move(tick));
            }
        };

        emit(data["b"].get_array(), TickSide::BID);
        emit(data["a"].get_array(), TickSide::ASK);
    }

    void parseTrade(simdjson::dom::object data, uint64_t recv_ts,
                    std::function<void(Tick&&)> onTick,
                    std::function<uint32_t(const std::string&)> resolveSymbolId) {
        std::string_view symbol = data["s"].get_string();
        uint32_t sid = resolveSymbolId(std::string(symbol));
        if (sid == 0) return;

        Tick tick{};
        tick.exchange_id = static_cast<uint32_t>(ExchangeId::BINANCE);
        tick.symbol_id = sid;
        tick.price = parseDecimal(data["p"].get_string());
        tick.quantity = parseDecimal(data["q"].get_string());
        tick.exchange_timestamp_us = data["T"].get_uint64() * 1000;
        tick.receive_timestamp_us = recv_ts;
        tick.side = data["m"].get_bool() ? TickSide::TRADE : TickSide::BID;
        tick.flags = 1;
        onTick(std::move(tick));
    }
};

BinanceJsonProtocol::BinanceJsonProtocol() : pimpl_(std::make_unique<Impl>()) {}
BinanceJsonProtocol::~BinanceJsonProtocol() = default;

size_t BinanceJsonProtocol::parse(
    const std::string& msg, uint64_t receive_ts,
    std::function<void(Tick&&)> onTick,
    std::function<uint32_t(const std::string&)> resolveSymbolId) {

    simdjson::dom::element doc;
    try {
        doc = pimpl_->json_parser.parse(msg);
    } catch (const simdjson::simdjson_error& e) {
        spdlog::error("Binance parse error: {}", e.what());
        return 0;
    }
    simdjson::dom::object obj = doc.get_object();

    // Combined stream (spot): {"stream":"...","data":{...}}
    if (obj["stream"].error() == simdjson::SUCCESS) {
        std::string_view sn = obj["stream"].get_string();
        simdjson::dom::object data = obj["data"].get_object();

        if (sn.find(DEPTH_SUFFIX) != std::string_view::npos) {
            pimpl_->parseDepthUpdate(data, receive_ts, onTick, resolveSymbolId);
            return 5; // approximate
        }
        if (sn.find(TRADE_SUFFIX) != std::string_view::npos) {
            pimpl_->parseTrade(data, receive_ts, onTick, resolveSymbolId);
            return 1;
        }
        // TICKER_SUFFIX → parse24hrTicker is a no-op
        return 0;
    }

    // Raw stream (futures): {"e":"depthUpdate","E":...,"s":"...",...}
    if (obj["e"].error() == simdjson::SUCCESS) {
        std::string_view event = obj["e"].get_string();
        if (event == "depthUpdate") {
            pimpl_->parseDepthUpdate(obj, receive_ts, onTick, resolveSymbolId);
            return 5;
        }
        if (event == "aggTrade" || event == "trade") {
            pimpl_->parseTrade(obj, receive_ts, onTick, resolveSymbolId);
            return 1;
        }
        return 0;
    }

    // Subscription response — not a data message
    return 0;
}

std::string BinanceJsonProtocol::subscribeRequest(const std::vector<std::string>& symbols) {
    static std::atomic<uint64_t> msg_id{1};
    nlohmann::json params = nlohmann::json::array();
    for (const auto& sym : symbols) {
        params.push_back(sym + DEPTH_SUFFIX);
        params.push_back(sym + TRADE_SUFFIX);
        params.push_back(sym + TICKER_SUFFIX);
    }
    return nlohmann::json{
        {"method", "SUBSCRIBE"},
        {"params", params},
        {"id", msg_id++}
    }.dump();
}

std::string BinanceJsonProtocol::unsubscribeRequest(const std::vector<std::string>& symbols) {
    static std::atomic<uint64_t> msg_id{1};
    nlohmann::json params = nlohmann::json::array();
    for (const auto& sym : symbols) {
        params.push_back(sym + DEPTH_SUFFIX);
        params.push_back(sym + TRADE_SUFFIX);
        params.push_back(sym + TICKER_SUFFIX);
    }
    return nlohmann::json{
        {"method", "UNSUBSCRIBE"},
        {"params", params},
        {"id", msg_id++}
    }.dump();
}

std::string BinanceJsonProtocol::normalizeSymbol(const std::string& raw) const {
    std::string n = raw;
    std::transform(n.begin(), n.end(), n.begin(), ::tolower);
    return n;
}

} // namespace market_data
} // namespace chronos
