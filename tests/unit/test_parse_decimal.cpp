#include <gtest/gtest.h>
#include <chronos/core/types.hpp>

namespace chronos {
namespace test {

TEST(ParseDecimalTest, BasicValues) {
    // Simple values
    EXPECT_EQ(parseDecimal("0").raw_value(), toDecimal(0.0).raw_value());
    EXPECT_EQ(parseDecimal("1").raw_value(), toDecimal(1.0).raw_value());
    EXPECT_EQ(parseDecimal("1234").raw_value(), toDecimal(1234.0).raw_value());
}

TEST(ParseDecimalTest, FractionalValues) {
    EXPECT_EQ(parseDecimal("0.5").raw_value(), toDecimal(0.5).raw_value());
    EXPECT_EQ(parseDecimal("1.25").raw_value(), toDecimal(1.25).raw_value());
    EXPECT_EQ(parseDecimal("1234.56").raw_value(), toDecimal(1234.56).raw_value());
}

TEST(ParseDecimalTest, NegativeValues) {
    EXPECT_EQ(parseDecimal("-1").raw_value(), toDecimal(-1.0).raw_value());
    EXPECT_EQ(parseDecimal("-0.5").raw_value(), toDecimal(-0.5).raw_value());
    EXPECT_EQ(parseDecimal("-1234.56").raw_value(), toDecimal(-1234.56).raw_value());
}

TEST(ParseDecimalTest, SmallValues) {
    EXPECT_GT(parseDecimal("0.001").raw_value(), 0);
    EXPECT_GT(parseDecimal("0.00000001").raw_value(), 0);
}

TEST(ParseDecimalTest, MaxPrecision) {
    // 8 decimal places — typical for crypto prices
    auto v = parseDecimal("1.12345678");
    EXPECT_GT(v.raw_value(), 0);
    EXPECT_LT(v.raw_value(), toDecimal(2.0).raw_value());
}

TEST(ParseDecimalTest, EdgeCases) {
    // Empty string
    EXPECT_EQ(parseDecimal("").raw_value(), 0);

    // Whitespace
    EXPECT_EQ(parseDecimal(" 123").raw_value(), toDecimal(123.0).raw_value());

    // Leading plus
    EXPECT_EQ(parseDecimal("+5").raw_value(), toDecimal(5.0).raw_value());

    // Trailing decimal point
    EXPECT_EQ(parseDecimal("1234.").raw_value(), toDecimal(1234.0).raw_value());

    // Leading decimal point
    EXPECT_EQ(parseDecimal(".5").raw_value(), toDecimal(0.5).raw_value());

    // Invalid input
    EXPECT_EQ(parseDecimal("abc").raw_value(), 0);
}

TEST(ParseDecimalTest, BinanceStylePrices) {
    // Real Binance-style price strings: BTCUSDT ~50000
    auto btc = parseDecimal("50012.34");
    EXPECT_NEAR(static_cast<double>(btc.raw_value()) / static_cast<double>(1ULL << 32),
                50012.34, 0.00001);

    // ETH ~3000
    auto eth = parseDecimal("3125.50");
    EXPECT_NEAR(static_cast<double>(eth.raw_value()) / static_cast<double>(1ULL << 32),
                3125.50, 0.00001);

    // Small altcoin
    auto doge = parseDecimal("0.12345");
    EXPECT_NEAR(static_cast<double>(doge.raw_value()) / static_cast<double>(1ULL << 32),
                0.12345, 0.00000001);

    // Quantity
    auto qty = parseDecimal("0.001");
    EXPECT_GT(qty.raw_value(), 0);
}

TEST(ParseDecimalTest, RoundTripWithToDecimal) {
    // parseDecimal should produce same result as toDecimal(stod(x))
    // for common price formats
    auto check_roundtrip = [](const char* s) {
        auto direct = parseDecimal(s);
        auto via_double = toDecimal(std::stod(s));
        // Within 1 bit of difference due to double precision limitations
        int64_t diff = direct.raw_value() - via_double.raw_value();
        EXPECT_LE(std::abs(diff), 2)
            << "parseDecimal(\"" << s << "\") differs from toDecimal(stod(\"" << s << "\"))";
    };

    check_roundtrip("1.0");
    check_roundtrip("0.5");
    check_roundtrip("100.25");
    check_roundtrip("0.001");
    check_roundtrip("50012.34");
    check_roundtrip("0.12345678");
}

} // namespace test
} // namespace chronos
