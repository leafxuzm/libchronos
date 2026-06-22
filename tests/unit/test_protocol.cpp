#include <gtest/gtest.h>
#include <chronos/io/protocols/protocol.hpp>
#include <chronos/io/protocols/binance_json_protocol.hpp>
#include <chronos/io/protocols/okx_json_protocol.hpp>
#include <chronos/core/types.hpp>
#include <vector>
#include <string>
#include <cstdint>

namespace chronos {
namespace test {

using namespace market_data;

// ============================================================================
// Helper: wrap Protocol::parse() output into a simple vector
// ============================================================================
struct TickCollector {
    std::vector<Tick> ticks;
    void operator()(Tick&& t) { ticks.push_back(std::move(t)); }
};

static uint32_t identityResolver(const std::string& sym) {
    if (sym.empty()) return 0;
    return static_cast<uint32_t>(sym[0]);
}

// ============================================================================
// BinanceJsonProtocol
// ============================================================================
class BinanceProtocolTest : public ::testing::Test {
protected:
    BinanceJsonProtocol proto_;
    static constexpr uint64_t kRecvTs = 1718000000000000ULL;
};

TEST_F(BinanceProtocolTest, ParseCombinedStreamDepth) {
    // Combined stream uses the same suffixes as subscribeRequest generates
    std::string msg = R"({"stream":"btcusdt@depth5@100ms","data":{
        "e":"depthUpdate","E":1718000000000,"s":"BTCUSDT",
        "b":[["50000.00","1.5"],["49900.00","2.0"]],
        "a":[["50100.00","0.5"],["50200.00","1.0"]]
    }})";

    TickCollector collector;
    // std::ref prevents std::function from copying the collector
    size_t n = proto_.parse(msg, kRecvTs, std::ref(collector), identityResolver);
    EXPECT_GT(n, 0u);
    ASSERT_GE(collector.ticks.size(), 2u);

    const auto& b0 = collector.ticks[0];
    EXPECT_EQ(b0.exchange_id, static_cast<uint32_t>(ExchangeId::BINANCE));
    EXPECT_EQ(b0.symbol_id, identityResolver("BTCUSDT"));
    EXPECT_EQ(b0.side, TickSide::BID);
    EXPECT_GT(toDouble(b0.price), 0.0);
    EXPECT_GT(toDouble(b0.quantity), 0.0);
    EXPECT_EQ(b0.receive_timestamp_us, kRecvTs);
    EXPECT_GT(b0.exchange_timestamp_us, 0ULL);

    bool found_ask = false;
    for (size_t i = 0; i < collector.ticks.size(); ++i) {
        if (collector.ticks[i].side == TickSide::ASK) {
            found_ask = true;
            EXPECT_GT(toDouble(collector.ticks[i].price), 0.0);
            break;
        }
    }
    EXPECT_TRUE(found_ask);
}

TEST_F(BinanceProtocolTest, ParseRawDepthUpdate) {
    std::string msg = R"({"e":"depthUpdate","E":1718000000000,"s":"ETHUSDT",
        "b":[["3000.00","10.0"],["2990.00","5.0"]],
        "a":[["3010.00","3.0"]]
    })";

    TickCollector collector;
    size_t n = proto_.parse(msg, kRecvTs, std::ref(collector), identityResolver);
    EXPECT_GT(n, 0u);
    ASSERT_GE(collector.ticks.size(), 3u);

    EXPECT_EQ(collector.ticks[0].side, TickSide::BID);
    EXPECT_EQ(collector.ticks[0].symbol_id, identityResolver("ETHUSDT"));
    EXPECT_EQ(collector.ticks[0].exchange_timestamp_us, 1718000000000ULL * 1000);
}

TEST_F(BinanceProtocolTest, ParseRawAggTrade) {
    std::string msg = R"({"e":"aggTrade","E":1718000000000,"s":"BTCUSDT",
        "p":"50000.00","q":"0.5","T":1718000000,"m":true})";

    TickCollector collector;
    size_t n = proto_.parse(msg, kRecvTs, std::ref(collector), identityResolver);
    EXPECT_EQ(n, 1u);
    ASSERT_EQ(collector.ticks.size(), 1u);

    const auto& t = collector.ticks[0];
    EXPECT_EQ(t.exchange_id, static_cast<uint32_t>(ExchangeId::BINANCE));
    EXPECT_EQ(t.side, TickSide::TRADE);  // m=true → taker sell → TRADE
    EXPECT_EQ(t.flags, 1u);
    EXPECT_GT(toDouble(t.price), 0.0);
    EXPECT_GT(toDouble(t.quantity), 0.0);
}

TEST_F(BinanceProtocolTest, ParseAggTradeBuySide) {
    std::string msg = R"({"e":"aggTrade","E":1718000000000,"s":"BTCUSDT",
        "p":"50000.00","q":"0.5","T":1718000000,"m":false})";

    TickCollector collector;
    proto_.parse(msg, kRecvTs, std::ref(collector), identityResolver);
    ASSERT_EQ(collector.ticks.size(), 1u);
    EXPECT_EQ(collector.ticks[0].side, TickSide::BID);
}

TEST_F(BinanceProtocolTest, ParseCombinedStreamTrade) {
    std::string msg = R"({"stream":"btcusdt@aggTrade","data":{
        "e":"aggTrade","E":1718000000000,"s":"BTCUSDT",
        "p":"50000.00","q":"1.0","T":1718000000,"m":false
    }})";

    TickCollector collector;
    size_t n = proto_.parse(msg, kRecvTs, std::ref(collector), identityResolver);
    EXPECT_EQ(n, 1u);
    ASSERT_EQ(collector.ticks.size(), 1u);
    EXPECT_EQ(collector.ticks[0].side, TickSide::BID);
}

TEST_F(BinanceProtocolTest, ParseTickerReturnsZero) {
    std::string msg = R"({"stream":"btcusdt@bookTicker","data":{
        "e":"24hrTicker","s":"BTCUSDT","c":"50000.00"
    }})";

    TickCollector collector;
    size_t n = proto_.parse(msg, kRecvTs, std::ref(collector), identityResolver);
    EXPECT_EQ(n, 0u);
    EXPECT_TRUE(collector.ticks.empty());
}

TEST_F(BinanceProtocolTest, UnknownSymbolDropped) {
    auto dropAll = [](const std::string&) -> uint32_t { return 0; };
    std::string msg = R"({"e":"aggTrade","E":1718000000000,"s":"UNKNOWN",
        "p":"1.00","q":"1.0","T":1718000000,"m":false})";

    TickCollector collector;
    size_t n = proto_.parse(msg, kRecvTs, std::ref(collector), dropAll);
    // parse returns 1 for recognised event type even though tick is dropped (sid==0)
    EXPECT_EQ(n, 1u);
    EXPECT_TRUE(collector.ticks.empty());
}

TEST_F(BinanceProtocolTest, UnknownEventReturnsZero) {
    std::string msg = R"({"e":"kline","E":1718000000000,"s":"BTCUSDT"})";
    TickCollector collector;
    size_t n = proto_.parse(msg, kRecvTs, std::ref(collector), identityResolver);
    EXPECT_EQ(n, 0u);
    EXPECT_TRUE(collector.ticks.empty());
}

TEST_F(BinanceProtocolTest, MalformedJsonReturnsZero) {
    std::string msg = "not json at all {{{";
    TickCollector collector;
    size_t n = proto_.parse(msg, kRecvTs, std::ref(collector), identityResolver);
    EXPECT_EQ(n, 0u);
    EXPECT_TRUE(collector.ticks.empty());
}

TEST_F(BinanceProtocolTest, EmptyObjectReturnsZero) {
    std::string msg = "{}";
    TickCollector collector;
    size_t n = proto_.parse(msg, kRecvTs, std::ref(collector), identityResolver);
    EXPECT_EQ(n, 0u);
}

TEST_F(BinanceProtocolTest, SubscribeRequestSingleSymbol) {
    std::string req = proto_.subscribeRequest({"btcusdt"});
    EXPECT_NE(req.find("SUBSCRIBE"), std::string::npos);
    EXPECT_NE(req.find("btcusdt@depth5@100ms"), std::string::npos);
    EXPECT_NE(req.find("btcusdt@aggTrade"), std::string::npos);
    EXPECT_NE(req.find("btcusdt@bookTicker"), std::string::npos);
    EXPECT_NE(req.find("\"id\""), std::string::npos);
}

TEST_F(BinanceProtocolTest, SubscribeRequestMultiSymbol) {
    std::string req = proto_.subscribeRequest({"btcusdt", "ethusdt"});
    EXPECT_NE(req.find("btcusdt@depth5@100ms"), std::string::npos);
    EXPECT_NE(req.find("btcusdt@aggTrade"), std::string::npos);
    EXPECT_NE(req.find("ethusdt@depth5@100ms"), std::string::npos);
    EXPECT_NE(req.find("ethusdt@aggTrade"), std::string::npos);
}

TEST_F(BinanceProtocolTest, UnsubscribeRequest) {
    std::string req = proto_.unsubscribeRequest({"btcusdt"});
    EXPECT_NE(req.find("UNSUBSCRIBE"), std::string::npos);
    EXPECT_NE(req.find("btcusdt@depth5@100ms"), std::string::npos);
}

TEST_F(BinanceProtocolTest, SubscribeRequestIdMonotonic) {
    std::string r1 = proto_.subscribeRequest({"x"});
    std::string r2 = proto_.subscribeRequest({"y"});
    EXPECT_NE(r1, r2);
}

TEST_F(BinanceProtocolTest, NormalizeSymbolLowercase) {
    EXPECT_EQ(proto_.normalizeSymbol("BTCUSDT"), "btcusdt");
    EXPECT_EQ(proto_.normalizeSymbol("EthUsdt"), "ethusdt");
    EXPECT_EQ(proto_.normalizeSymbol("BTC-USDT"), "btc-usdt");
}

TEST_F(BinanceProtocolTest, HeartbeatBehavior) {
    EXPECT_TRUE(proto_.usesWsPing());
    EXPECT_FALSE(proto_.isPingMessage("anything"));
    EXPECT_FALSE(proto_.isPongMessage("anything"));
    EXPECT_TRUE(proto_.heartbeatPayload().empty());
}

TEST_F(BinanceProtocolTest, DefaultUrl) {
    EXPECT_NE(proto_.defaultUrl().find("binance"), std::string::npos);
}

// ============================================================================
// OKXJsonProtocol
// ============================================================================
class OKXProtocolTest : public ::testing::Test {
protected:
    OKXJsonProtocol proto_;
    static constexpr uint64_t kRecvTs = 1718000000000000ULL;
};

TEST_F(OKXProtocolTest, ParseBooks5Data) {
    std::string msg = R"({"arg":{"channel":"books5","instId":"BTC-USDT"},"data":[{
        "ts":"1718000000000",
        "bids":[["50000.00","1.5","0","1"]],
        "asks":[["50100.00","0.5","0","1"]],
        "checksum":123
    }]})";

    TickCollector collector;
    size_t n = proto_.parse(msg, kRecvTs, std::ref(collector), identityResolver);
    EXPECT_GT(n, 0u);
    ASSERT_GE(collector.ticks.size(), 2u);

    const auto& bid = collector.ticks[0];
    EXPECT_EQ(bid.exchange_id, static_cast<uint32_t>(ExchangeId::OKX));
    EXPECT_EQ(bid.symbol_id, identityResolver("BTC-USDT"));
    EXPECT_EQ(bid.side, TickSide::BID);
    EXPECT_GT(toDouble(bid.price), 0.0);
    EXPECT_GT(bid.exchange_timestamp_us, 0ULL);

    bool has_ask = false;
    for (const auto& t : collector.ticks) {
        if (t.side == TickSide::ASK) { has_ask = true; break; }
    }
    EXPECT_TRUE(has_ask);
}

TEST_F(OKXProtocolTest, ParseBooks5MultipleSnapshots) {
    std::string msg = R"({"arg":{"channel":"books5","instId":"ETH-USDT"},"data":[
        {"ts":"1718000000000","bids":[["3000.00","10.0","0","1"]],"asks":[]},
        {"ts":"1718000000001","bids":[["3001.00","5.0","0","1"]],"asks":[["3002.00","2.0","0","1"]]}
    ]})";

    TickCollector collector;
    proto_.parse(msg, kRecvTs, std::ref(collector), identityResolver);
    EXPECT_GE(collector.ticks.size(), 3u);
}

TEST_F(OKXProtocolTest, ParseTradesData) {
    std::string msg = R"({"arg":{"channel":"trades","instId":"BTC-USDT"},"data":[{
        "side":"buy","px":"50000.00","sz":"0.5","ts":"1718000000000"
    }]})";

    TickCollector collector;
    proto_.parse(msg, kRecvTs, std::ref(collector), identityResolver);
    ASSERT_EQ(collector.ticks.size(), 1u);

    const auto& t = collector.ticks[0];
    EXPECT_EQ(t.exchange_id, static_cast<uint32_t>(ExchangeId::OKX));
    EXPECT_EQ(t.side, TickSide::BID);
    EXPECT_EQ(t.flags, 1u);
    EXPECT_GT(toDouble(t.price), 0.0);
    EXPECT_GT(toDouble(t.quantity), 0.0);
}

TEST_F(OKXProtocolTest, ParseTradesSellSide) {
    std::string msg = R"({"arg":{"channel":"trades","instId":"BTC-USDT"},"data":[{
        "side":"sell","px":"50000.00","sz":"0.5","ts":"1718000000000"
    }]})";

    TickCollector collector;
    proto_.parse(msg, kRecvTs, std::ref(collector), identityResolver);
    ASSERT_EQ(collector.ticks.size(), 1u);
    EXPECT_EQ(collector.ticks[0].side, TickSide::TRADE);
}

TEST_F(OKXProtocolTest, ParseSubscribeEvent) {
    std::string msg = R"({"event":"subscribe","arg":{"channel":"books5","instId":"BTC-USDT"}})";
    TickCollector collector;
    size_t n = proto_.parse(msg, kRecvTs, std::ref(collector), identityResolver);
    EXPECT_EQ(n, 0u);
    EXPECT_TRUE(collector.ticks.empty());
}

TEST_F(OKXProtocolTest, ParseErrorEvent) {
    std::string msg = R"({"event":"error","msg":"invalid channel","code":"60000"})";
    TickCollector collector;
    size_t n = proto_.parse(msg, kRecvTs, std::ref(collector), identityResolver);
    EXPECT_EQ(n, 0u);
    EXPECT_TRUE(collector.ticks.empty());
}

TEST_F(OKXProtocolTest, UnknownChannelReturnsZero) {
    std::string msg = R"({"arg":{"channel":"mark-price","instId":"BTC-USDT"},"data":[]})";
    TickCollector collector;
    size_t n = proto_.parse(msg, kRecvTs, std::ref(collector), identityResolver);
    EXPECT_EQ(n, 0u);
}

TEST_F(OKXProtocolTest, UnknownSymbolDropped) {
    auto dropAll = [](const std::string&) -> uint32_t { return 0; };
    std::string msg = R"({"arg":{"channel":"trades","instId":"UNKNOWN"},"data":[{
        "side":"buy","px":"1.00","sz":"1.0","ts":"1718000000000"
    }]})";

    TickCollector collector;
    // parse returns 1 for recognised channel even though tick is dropped (sid==0)
    size_t n = proto_.parse(msg, kRecvTs, std::ref(collector), dropAll);
    EXPECT_EQ(n, 1u);
    EXPECT_TRUE(collector.ticks.empty());
}

TEST_F(OKXProtocolTest, MalformedJsonReturnsZero) {
    std::string msg = "not valid {{{";
    TickCollector collector;
    size_t n = proto_.parse(msg, kRecvTs, std::ref(collector), identityResolver);
    EXPECT_EQ(n, 0u);
}

TEST_F(OKXProtocolTest, EmptyDataArrayReturnsZero) {
    std::string msg = R"({"arg":{"channel":"books5","instId":"BTC-USDT"},"data":[]})";
    TickCollector collector;
    size_t n = proto_.parse(msg, kRecvTs, std::ref(collector), identityResolver);
    EXPECT_EQ(n, 0u);
}

TEST_F(OKXProtocolTest, SubscribeRequestSingleSymbol) {
    std::string req = proto_.subscribeRequest({"BTC-USDT"});
    EXPECT_NE(req.find("subscribe"), std::string::npos);
    EXPECT_NE(req.find("books5"), std::string::npos);
    EXPECT_NE(req.find("trades"), std::string::npos);
    EXPECT_NE(req.find("BTC-USDT"), std::string::npos);
}

TEST_F(OKXProtocolTest, SubscribeRequestMultiSymbol) {
    std::string req = proto_.subscribeRequest({"BTC-USDT", "ETH-USDT"});
    EXPECT_NE(req.find("BTC-USDT"), std::string::npos);
    EXPECT_NE(req.find("books5"), std::string::npos);
    EXPECT_NE(req.find("ETH-USDT"), std::string::npos);
}

TEST_F(OKXProtocolTest, UnsubscribeRequest) {
    std::string req = proto_.unsubscribeRequest({"BTC-USDT"});
    EXPECT_NE(req.find("unsubscribe"), std::string::npos);
    EXPECT_NE(req.find("BTC-USDT"), std::string::npos);
}

TEST_F(OKXProtocolTest, NormalizeSymbolUpperAndDash) {
    EXPECT_EQ(proto_.normalizeSymbol("btc-usdt"), "BTC-USDT");
    EXPECT_EQ(proto_.normalizeSymbol("btc_usdt"), "BTC-USDT");
    EXPECT_EQ(proto_.normalizeSymbol("eth_usdt"), "ETH-USDT");
    EXPECT_EQ(proto_.normalizeSymbol("BTC-USDT"), "BTC-USDT");
}

TEST_F(OKXProtocolTest, HeartbeatBehavior) {
    EXPECT_FALSE(proto_.usesWsPing());
    EXPECT_TRUE(proto_.isPingMessage("ping"));
    EXPECT_FALSE(proto_.isPingMessage("pong"));
    EXPECT_FALSE(proto_.isPingMessage("other"));
    EXPECT_TRUE(proto_.isPongMessage("pong"));
    EXPECT_FALSE(proto_.isPongMessage("ping"));
    EXPECT_EQ(proto_.heartbeatPayload(), "ping");
}

TEST_F(OKXProtocolTest, DefaultUrl) {
    EXPECT_NE(proto_.defaultUrl().find("okx"), std::string::npos);
}

} // namespace test
} // namespace chronos
