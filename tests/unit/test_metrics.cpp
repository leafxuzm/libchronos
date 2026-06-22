#include <gtest/gtest.h>
#include <cmath>
#include <filesystem>
#include <fstream>

#include "chronos/backtest/metrics.hpp"
#include "chronos/core/types.hpp"

using namespace chronos;
using namespace chronos::backtest;

namespace fs = std::filesystem;

// ============================================================================
// Helpers
// ============================================================================

namespace {

Trade makeTrade(double pnl, double qty = 0.1) {
    Trade t;
    t.entry_price = toDecimal(100.0);
    t.exit_price = toDecimal(100.0 + pnl / (qty > 0 ? qty : 0.1));
    t.quantity = toDecimal(qty);
    t.pnl = toDecimal(pnl);
    t.direction = OrderSide::BUY;
    t.entry_time_us = 1000;
    t.exit_time_us = 2000;
    return t;
}

const double EPS = 1e-6;

}  // namespace

// ============================================================================
// 1. Trade Recording
// ============================================================================

TEST(MetricsCollectorTest, InitiallyZero) {
    MetricsCollector mc;
    EXPECT_EQ(mc.totalTrades(), 0u);
    EXPECT_EQ(mc.winningTrades(), 0u);
    EXPECT_EQ(mc.losingTrades(), 0u);
    // Before calculateMetrics, values are defaults
    EXPECT_NEAR(toDouble(mc.totalReturn()), 0.0, EPS);
}

TEST(MetricsCollectorTest, RecordSingleTrade) {
    MetricsCollector mc;
    mc.recordTrade(makeTrade(5.0));
    EXPECT_EQ(mc.totalTrades(), 1u);
}

TEST(MetricsCollectorTest, RecordEquity) {
    MetricsCollector mc;
    mc.recordEquity(1000000, toDecimal(10000.0));
    mc.recordEquity(2000000, toDecimal(10500.0));
    mc.calculateMetrics();

    EXPECT_GT(mc.equityCurve().size(), 0u);
    EXPECT_GT(mc.drawdownCurve().size(), 0u);
}

// ============================================================================
// 2. Total Return
// ============================================================================

TEST(MetricsCollectorTest, TotalReturnPositive) {
    MetricsCollector mc;
    mc.recordEquity(0, toDecimal(10000.0));
    mc.recordEquity(1000000, toDecimal(11000.0));
    mc.calculateMetrics();

    double ret = toDouble(mc.totalReturn());
    EXPECT_NEAR(ret, 0.10, EPS);
}

TEST(MetricsCollectorTest, TotalReturnNegative) {
    MetricsCollector mc;
    mc.recordEquity(0, toDecimal(10000.0));
    mc.recordEquity(1000000, toDecimal(9000.0));
    mc.calculateMetrics();

    double ret = toDouble(mc.totalReturn());
    EXPECT_NEAR(ret, -0.10, EPS);
}

// ============================================================================
// 3. Win Rate
// ============================================================================

TEST(MetricsCollectorTest, WinRateAllWins) {
    MetricsCollector mc;
    mc.recordTrade(makeTrade(10.0));
    mc.recordTrade(makeTrade(20.0));
    mc.recordTrade(makeTrade(5.0));
    mc.calculateMetrics();

    EXPECT_EQ(mc.winningTrades(), 3u);
    EXPECT_EQ(mc.losingTrades(), 0u);
    EXPECT_NEAR(mc.winRate(), 1.0, EPS);
}

TEST(MetricsCollectorTest, WinRateMixed) {
    MetricsCollector mc;
    mc.recordTrade(makeTrade(10.0));
    mc.recordTrade(makeTrade(-5.0));
    mc.recordTrade(makeTrade(3.0));
    mc.recordTrade(makeTrade(-2.0));
    mc.calculateMetrics();

    EXPECT_EQ(mc.winningTrades(), 2u);
    EXPECT_EQ(mc.losingTrades(), 2u);
    EXPECT_NEAR(mc.winRate(), 0.50, EPS);
}

TEST(MetricsCollectorTest, WinRateAllLosses) {
    MetricsCollector mc;
    mc.recordTrade(makeTrade(-10.0));
    mc.recordTrade(makeTrade(-5.0));
    mc.calculateMetrics();

    EXPECT_EQ(mc.winningTrades(), 0u);
    EXPECT_EQ(mc.losingTrades(), 2u);
    EXPECT_NEAR(mc.winRate(), 0.0, EPS);
}

// ============================================================================
// 4. Profit Factor
// ============================================================================

TEST(MetricsCollectorTest, ProfitFactorGreaterThanOne) {
    MetricsCollector mc;
    mc.recordTrade(makeTrade(20.0));
    mc.recordTrade(makeTrade(-5.0));
    mc.calculateMetrics();

    EXPECT_GT(mc.profitFactor(), 1.0);
    EXPECT_NEAR(mc.profitFactor(), 4.0, EPS);
}

TEST(MetricsCollectorTest, ProfitFactorLessThanOne) {
    MetricsCollector mc;
    mc.recordTrade(makeTrade(5.0));
    mc.recordTrade(makeTrade(-20.0));
    mc.calculateMetrics();

    EXPECT_LT(mc.profitFactor(), 1.0);
    EXPECT_NEAR(mc.profitFactor(), 0.25, EPS);
}

TEST(MetricsCollectorTest, ProfitFactorNoLosses) {
    MetricsCollector mc;
    mc.recordTrade(makeTrade(10.0));
    mc.recordTrade(makeTrade(5.0));
    mc.calculateMetrics();

    EXPECT_TRUE(std::isinf(mc.profitFactor()));
    EXPECT_GT(mc.profitFactor(), 0.0);
}

// ============================================================================
// 5. Average Win / Loss
// ============================================================================

TEST(MetricsCollectorTest, AverageWinAndLoss) {
    MetricsCollector mc;
    mc.recordTrade(makeTrade(10.0));   // win
    mc.recordTrade(makeTrade(20.0));   // win
    mc.recordTrade(makeTrade(-5.0));   // loss
    mc.recordTrade(makeTrade(-15.0));  // loss
    mc.calculateMetrics();

    EXPECT_NEAR(toDouble(mc.avgWin()), 15.0, 1e-9);
    EXPECT_NEAR(toDouble(mc.avgLoss()), -10.0, 1e-9);
}

// ============================================================================
// 6. Drawdown
// ============================================================================

TEST(MetricsCollectorTest, MaxDrawdownFromPeak) {
    MetricsCollector mc;
    mc.recordEquity(0,        toDecimal(10000.0));  // peak
    mc.recordEquity(1000000,  toDecimal(9500.0));   // drawdown -5%
    mc.recordEquity(2000000,  toDecimal(9800.0));   // recover
    mc.recordEquity(3000000,  toDecimal(8500.0));   // deeper drawdown -15%
    mc.recordEquity(4000000,  toDecimal(11000.0));  // new high
    mc.calculateMetrics();

    // Max drawdown should be from 10000 peak to 8500 trough
    double dd_pct = mc.maxDrawdownPct();
    EXPECT_NEAR(dd_pct, 15.0, 0.1);
}

TEST(MetricsCollectorTest, NoDrawdownWhenRising) {
    MetricsCollector mc;
    mc.recordEquity(0,        toDecimal(10000.0));
    mc.recordEquity(1000000,  toDecimal(10500.0));
    mc.recordEquity(2000000,  toDecimal(11000.0));
    mc.calculateMetrics();

    EXPECT_NEAR(mc.maxDrawdownPct(), 0.0, EPS);
    EXPECT_EQ(toDouble(mc.maxDrawdown()), 0.0);
}

// ============================================================================
// 7. Sharpe Ratio
// ============================================================================

TEST(MetricsCollectorTest, SharpeRatioPositiveReturns) {
    MetricsCollector mc;
    // Steady 1% daily returns over a few days
    constexpr uint64_t DAY = 86'400'000'000ULL;
    double eq = 10000.0;
    for (int d = 0; d < 10; ++d) {
        mc.recordEquity(d * DAY, toDecimal(eq));
        eq *= 1.01;  // 1% per day
    }
    mc.calculateMetrics();

    // Positive Sharpe (low risk, steady returns)
    EXPECT_GT(mc.sharpeRatio(), 0.0);
}

TEST(MetricsCollectorTest, SharpeRatioFlatReturns) {
    MetricsCollector mc;
    constexpr uint64_t DAY = 86'400'000'000ULL;
    for (int d = 0; d < 5; ++d) {
        mc.recordEquity(d * DAY, toDecimal(10000.0));
    }
    mc.calculateMetrics();

    // No variability → Sharpe ≈ 0 or NaN
    EXPECT_LE(mc.sharpeRatio(), 0.0);
}

// ============================================================================
// 8. JSON / CSV Export
// ============================================================================

TEST(MetricsCollectorTest, ExportToJSON) {
    MetricsCollector mc;
    mc.recordEquity(0, toDecimal(10000.0));
    mc.recordEquity(1000000, toDecimal(11000.0));
    mc.recordTrade(makeTrade(10.0));
    mc.calculateMetrics();

    auto j = mc.exportToJSON();
    EXPECT_TRUE(j.contains("total_return"));
    EXPECT_TRUE(j.contains("sharpe_ratio"));
    EXPECT_TRUE(j.contains("win_rate"));
    EXPECT_TRUE(j.contains("equity_curve"));
    EXPECT_TRUE(j.contains("drawdown_curve"));
    EXPECT_GT(j["equity_curve"].size(), 0u);
}

TEST(MetricsCollectorTest, ExportToCSV) {
    MetricsCollector mc;
    mc.recordEquity(0, toDecimal(10000.0));
    mc.recordEquity(1000000, toDecimal(11000.0));
    mc.recordTrade(makeTrade(10.0));
    mc.calculateMetrics();

    std::string csv = mc.exportToCSV();
    EXPECT_FALSE(csv.empty());
    EXPECT_NE(csv.find("total_return"), std::string::npos);
    EXPECT_NE(csv.find("equity"), std::string::npos);
}

// ============================================================================
// 9. Edge Cases
// ============================================================================

TEST(MetricsCollectorTest, NoData) {
    MetricsCollector mc;
    mc.calculateMetrics();  // Should not crash

    EXPECT_NEAR(toDouble(mc.totalReturn()), 0.0, EPS);
    EXPECT_EQ(mc.totalTrades(), 0u);
    EXPECT_EQ(mc.equityCurve().size(), 0u);
}

TEST(MetricsCollectorTest, SingleEquityPoint) {
    MetricsCollector mc;
    mc.recordEquity(0, toDecimal(10000.0));
    mc.calculateMetrics();

    EXPECT_EQ(mc.equityCurve().size(), 1u);
    EXPECT_NEAR(toDouble(mc.totalReturn()), 0.0, EPS);
}

TEST(MetricsCollectorTest, BreakevenTradesIgnoredInWinLoss) {
    MetricsCollector mc;
    mc.recordTrade(makeTrade(0.0));   // breakeven
    mc.recordTrade(makeTrade(10.0));  // win
    mc.recordTrade(makeTrade(-5.0));  // loss
    mc.calculateMetrics();

    EXPECT_EQ(mc.winningTrades(), 1u);
    EXPECT_EQ(mc.losingTrades(), 1u);
    EXPECT_EQ(mc.totalTrades(), 3u);
}

TEST(MetricsCollectorTest, DrawdownCurveLengthMatchesEquity) {
    MetricsCollector mc;
    constexpr uint64_t DAY = 86'400'000'000ULL;
    for (int d = 0; d < 5; ++d) {
        mc.recordEquity(d * DAY, toDecimal(10000.0 + d * 200));
    }
    mc.calculateMetrics();

    EXPECT_EQ(mc.drawdownCurve().size(), mc.equityCurve().size());
}
