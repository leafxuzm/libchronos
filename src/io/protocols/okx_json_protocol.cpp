#include <chronos/io/protocols/okx_json_protocol.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>

namespace chronos {
namespace market_data {

struct OKXJsonProtocol::Impl {
    simdjson::dom::parser json_parser;

    void parseBooks5(simdjson::dom::object data, const std::string& inst_id,
                     uint64_t recv_ts,
                     std::function<void(Tick&&)> onTick,
                     std::function<uint32_t(const std::string&)> resolveSymbolId) {
        uint32_t sid = resolveSymbolId(inst_id);
        if (sid == 0) return;

        uint64_t exchange_ts = recv_ts;
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
                tick.receive_timestamp_us = recv_ts;
                tick.symbol_id = sid;
                tick.price = parseDecimal(pair.at(0).get_string());
                tick.quantity = parseDecimal(pair.at(1).get_string());
                tick.side = side;
                tick.flags = 0;
                onTick(std::move(tick));
            }
        };

        emit(data["bids"].get_array(), TickSide::BID);
        emit(data["asks"].get_array(), TickSide::ASK);
    }

    void parseTrade(simdjson::dom::object data, const std::string& inst_id,
                    uint64_t recv_ts,
                    std::function<void(Tick&&)> onTick,
                    std::function<uint32_t(const std::string&)> resolveSymbolId) {
        uint32_t sid = resolveSymbolId(inst_id);
        if (sid == 0) return;

        std::string_view side_str = data["side"].get_string();
        TickSide side = (side_str == "buy") ? TickSide::BID : TickSide::TRADE;

        Tick tick{};
        tick.exchange_id = static_cast<uint32_t>(ExchangeId::OKX);
        std::string_view ts_sv = data["ts"].get_string();
        tick.exchange_timestamp_us = std::stoull(std::string(ts_sv)) * 1000;
        tick.receive_timestamp_us = recv_ts;
        tick.symbol_id = sid;
        tick.price = parseDecimal(data["px"].get_string());
        tick.quantity = parseDecimal(data["sz"].get_string());
        tick.side = side;
        tick.flags = 1;
        onTick(std::move(tick));
    }
};

OKXJsonProtocol::OKXJsonProtocol() : pimpl_(std::make_unique<Impl>()) {}
OKXJsonProtocol::~OKXJsonProtocol() = default;

size_t OKXJsonProtocol::parse(
    const std::string& msg, uint64_t receive_ts,
    std::function<void(Tick&&)> onTick,
    std::function<uint32_t(const std::string&)> resolveSymbolId) {

    simdjson::dom::element doc;
    try {
        doc = pimpl_->json_parser.parse(msg);
    } catch (const simdjson::simdjson_error& e) {
        spdlog::error("OKX parse error: {}", e.what());
        return 0;
    }
    simdjson::dom::object obj = doc.get_object();

    // Event message: {"event":"subscribe",...} or {"event":"error",...}
    if (obj["event"].error() == simdjson::SUCCESS) {
        std::string_view event = obj["event"].get_string();
        if (event == "subscribe") {
            spdlog::debug("OKX subscription confirmed");
        } else if (event == "error") {
            std::string_view em = obj["msg"].get_string();
            spdlog::warn("OKX error: {}", std::string(em.data(), em.size()));
        }
        return 0;
    }

    // Data push: {"arg":{"channel":"...","instId":"..."},"data":[...]}
    if (obj["arg"].error() == simdjson::SUCCESS) {
        simdjson::dom::object arg = obj["arg"].get_object();
        std::string_view channel = arg["channel"].get_string();
        std::string_view inst_sv = arg["instId"].get_string();
        std::string inst_id(inst_sv);

        size_t count = 0;
        if (channel == "books5") {
            for (auto elem : obj["data"].get_array()) {
                pimpl_->parseBooks5(elem.get_object(), inst_id,
                                    receive_ts, onTick, resolveSymbolId);
                count += 5;
            }
        } else if (channel == "trades") {
            for (auto elem : obj["data"].get_array()) {
                pimpl_->parseTrade(elem.get_object(), inst_id,
                                   receive_ts, onTick, resolveSymbolId);
                count++;
            }
        }
        return count;
    }

    return 0;
}

std::string OKXJsonProtocol::subscribeRequest(const std::vector<std::string>& symbols) {
    nlohmann::json args = nlohmann::json::array();
    for (const auto& sym : symbols) {
        args.push_back({{"channel", "books5"}, {"instId", sym}});
        args.push_back({{"channel", "trades"}, {"instId", sym}});
    }
    return nlohmann::json{{"op", "subscribe"}, {"args", args}}.dump();
}

std::string OKXJsonProtocol::unsubscribeRequest(const std::vector<std::string>& symbols) {
    nlohmann::json args = nlohmann::json::array();
    for (const auto& sym : symbols) {
        args.push_back({{"channel", "books5"}, {"instId", sym}});
        args.push_back({{"channel", "trades"}, {"instId", sym}});
    }
    return nlohmann::json{{"op", "unsubscribe"}, {"args", args}}.dump();
}

std::string OKXJsonProtocol::normalizeSymbol(const std::string& raw) const {
    std::string n = raw;
    std::transform(n.begin(), n.end(), n.begin(), ::toupper);
    std::replace(n.begin(), n.end(), '_', '-');
    return n;
}

} // namespace market_data
} // namespace chronos
