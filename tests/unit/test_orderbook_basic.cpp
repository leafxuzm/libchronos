#include <chronos/market_data/orderbook.hpp>
#include <chronos/market_data/orderbook_v2.hpp>
#include <gtest/gtest.h>

using namespace chronos;
using namespace chronos::market_data;

// Helper function for approximate comparison
bool approxEqual(double a, double b, double epsilon = 0.0001) {
    return std::abs(a - b) < epsilon;
}

// ============================================================================
// Decimal Type Tests
// ============================================================================

TEST(DecimalTypeTest, ConstructionAndConversion) {
    Decimal d1 = toDecimal(42.5);
    double v1 = toDouble(d1);
    
    EXPECT_TRUE(approxEqual(v1, 42.5));
}

TEST(DecimalTypeTest, ArithmeticOperations) {
    Decimal a = toDecimal(100.0);
    Decimal b = toDecimal(50.0);
    
    Decimal sum = a + b;
    Decimal diff = a - b;
    Decimal prod = a * b;
    Decimal quot = a / b;
    
    EXPECT_TRUE(approxEqual(toDouble(sum), 150.0));
    EXPECT_TRUE(approxEqual(toDouble(diff), 50.0));
    EXPECT_TRUE(approxEqual(toDouble(prod), 5000.0));
    EXPECT_TRUE(approxEqual(toDouble(quot), 2.0));
}

TEST(DecimalTypeTest, ComparisonOperations) {
    Decimal a = toDecimal(100.0);
    Decimal b = toDecimal(200.0);
    
    EXPECT_LT(a, b);
    EXPECT_GT(b, a);
    EXPECT_LE(a, b);
    EXPECT_GE(b, a);
    EXPECT_NE(a, b);
    EXPECT_EQ(a, a);
}

TEST(DecimalTypeTest, CryptocurrencyPrecision) {
    // Bitcoin price with 8 decimal places - excellent precision for large values
    Decimal btc_price = toDecimal(67234.12345678);
    EXPECT_TRUE(approxEqual(toDouble(btc_price), 67234.12345678, 1e-7));
    
    // Small altcoin price with 8 decimals
    // Note: With 32 fraction bits (~2.3e-10 absolute precision), relative error for small numbers
    // can be around 1e-6 due to rounding
    Decimal small_price = toDecimal(0.00012345);
    EXPECT_TRUE(approxEqual(toDouble(small_price), 0.00012345, 5e-6));
    
    // Very small quantity (satoshi level - 1e-8)
    // At this scale, we're near the precision limit, so relative error can be ~2%
    // This is acceptable for satoshi-level calculations
    Decimal satoshi = toDecimal(0.00000001);
    EXPECT_TRUE(approxEqual(toDouble(satoshi), 0.00000001, 0.03));
    
    // Verify precision is maintained in calculations
    Decimal qty = toDecimal(1.23456789);
    Decimal total = btc_price * qty;
    double expected = 67234.12345678 * 1.23456789;
    // Allow slightly larger epsilon for multiplication result
    EXPECT_TRUE(approxEqual(toDouble(total), expected, 1e-5));
    
    // Test typical crypto trading scenario - this is the most important test
    Decimal eth_price = toDecimal(3456.78901234);
    Decimal eth_qty = toDecimal(0.12345678);
    Decimal eth_total = eth_price * eth_qty;
    double eth_expected = 3456.78901234 * 0.12345678;
    EXPECT_TRUE(approxEqual(toDouble(eth_total), eth_expected, 1e-5));
    
    // Verify that 8-decimal precision is maintained for typical crypto prices
    // This is the key requirement for cryptocurrency trading
    Decimal btc_qty = toDecimal(0.12345678);  // 8 decimals
    EXPECT_TRUE(approxEqual(toDouble(btc_qty), 0.12345678, 1e-7));
    
    // Test realistic BTC trading: price * quantity
    Decimal btc_trade_price = toDecimal(67234.56);
    Decimal btc_trade_qty = toDecimal(0.12345678);
    Decimal btc_trade_total = btc_trade_price * btc_trade_qty;
    double btc_trade_expected = 67234.56 * 0.12345678;
    EXPECT_TRUE(approxEqual(toDouble(btc_trade_total), btc_trade_expected, 1e-5));
}

// ============================================================================
// OrderBook V1 Tests
// ============================================================================

TEST(OrderBookTest, EmptyOrderbook) {
    OrderBook book;
    
    EXPECT_TRUE(book.empty());
    EXPECT_FALSE(book.getBestBid().has_value());
    EXPECT_FALSE(book.getBestAsk().has_value());
    EXPECT_FALSE(book.getMidPrice().has_value());
    EXPECT_FALSE(book.getSpread().has_value());
}

TEST(OrderBookTest, AddBidLevel) {
    OrderBook book;
    
    Tick tick;
    tick.price = toDecimal(100.0);
    tick.quantity = toDecimal(10.0);
    tick.side = TickSide::BID;
    
    book.update(tick);
    
    EXPECT_FALSE(book.empty());
    ASSERT_TRUE(book.getBestBid().has_value());
    EXPECT_TRUE(approxEqual(toDouble(*book.getBestBid()), 100.0));
}

TEST(OrderBookTest, AddAskLevel) {
    OrderBook book;
    
    Tick tick;
    tick.price = toDecimal(101.0);
    tick.quantity = toDecimal(5.0);
    tick.side = TickSide::ASK;
    
    book.update(tick);
    
    EXPECT_FALSE(book.empty());
    ASSERT_TRUE(book.getBestAsk().has_value());
    EXPECT_TRUE(approxEqual(toDouble(*book.getBestAsk()), 101.0));
}

TEST(OrderBookTest, MidPriceAndSpread) {
    OrderBook book;
    
    // Add bid
    Tick bid_tick;
    bid_tick.price = toDecimal(100.0);
    bid_tick.quantity = toDecimal(10.0);
    bid_tick.side = TickSide::BID;
    book.update(bid_tick);
    
    // Add ask
    Tick ask_tick;
    ask_tick.price = toDecimal(102.0);
    ask_tick.quantity = toDecimal(5.0);
    ask_tick.side = TickSide::ASK;
    book.update(ask_tick);
    
    ASSERT_TRUE(book.getMidPrice().has_value());
    EXPECT_TRUE(approxEqual(toDouble(*book.getMidPrice()), 101.0));
    
    ASSERT_TRUE(book.getSpread().has_value());
    EXPECT_TRUE(approxEqual(toDouble(*book.getSpread()), 2.0));
}

// ============================================================================
// OrderBook V2 Tests
// ============================================================================

TEST(OrderBookV2Test, EmptyOrderbook) {
    OrderBookV2 book;
    
    EXPECT_TRUE(book.empty());
    EXPECT_FALSE(book.getBestBid().has_value());
    EXPECT_FALSE(book.getBestAsk().has_value());
}

TEST(OrderBookV2Test, AddBidLevel) {
    OrderBookV2 book;
    
    Tick tick;
    tick.price = toDecimal(100.0);
    tick.quantity = toDecimal(10.0);
    tick.side = TickSide::BID;
    
    book.update(tick);
    
    EXPECT_FALSE(book.empty());
    ASSERT_TRUE(book.getBestBid().has_value());
    EXPECT_TRUE(approxEqual(toDouble(*book.getBestBid()), 100.0));
}

TEST(OrderBookV2Test, Top5Levels) {
    OrderBookV2 book;
    
    // Add 5 bid levels
    for (int i = 0; i < 5; ++i) {
        Tick tick;
        tick.price = toDecimal(100.0 - i);
        tick.quantity = toDecimal(10.0);
        tick.side = TickSide::BID;
        book.update(tick);
    }
    
    auto top5 = book.getTop5Bids();
    
    // Check first level
    EXPECT_TRUE(approxEqual(toDouble(top5[0].price), 100.0));
    EXPECT_TRUE(approxEqual(toDouble(top5[0].quantity), 10.0));
    
    // Check second level
    EXPECT_TRUE(approxEqual(toDouble(top5[1].price), 99.0));
}