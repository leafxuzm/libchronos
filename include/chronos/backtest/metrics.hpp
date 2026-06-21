#pragma once

#include "chronos/core/types.hpp"
#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace chronos {
namespace backtest {

// ============================================================================
// Trade record — one completed round-trip (open → close)
// ============================================================================

struct Trade {
    uint64_t entry_time_us;
    uint64_t exit_time_us;
    uint32_t symbol_id;
    Decimal entry_price;
    Decimal exit_price;
    Decimal quantity;
    Decimal pnl;            // signed P&L for this trade
    OrderSide direction;    // BUY = long, SELL = short
};

// ============================================================================
// MetricsCollector — trading performance metrics
// ============================================================================

class MetricsCollector {
public:
    MetricsCollector() = default;

    // --- Data Input ---

    /// Record a completed trade (round-trip).
    void recordTrade(const Trade& trade);

    /// Record an equity snap at a given timestamp (for equity curve).
    void recordEquity(uint64_t timestamp_us, Decimal equity);

    // --- Metrics Calculation ---

    /// Compute all metrics. Call after all data is recorded.
    void calculateMetrics();

    // --- Accessors (valid after calculateMetrics) ---

    // Returns
    Decimal totalReturn()       const { return total_return_; }
    double   annualizedReturn() const { return annualized_return_; }
    double   sharpeRatio()      const { return sharpe_ratio_; }
    double   sortinoRatio()     const { return sortino_ratio_; }
    double   calmarRatio()      const { return calmar_ratio_; }

    // Drawdown
    Decimal maxDrawdown()       const { return max_drawdown_; }
    double   maxDrawdownPct()   const { return max_drawdown_pct_; }
    uint64_t maxDrawdownDurationUs() const { return max_dd_duration_us_; }

    // Trade stats
    double   winRate()          const { return win_rate_; }
    double   profitFactor()     const { return profit_factor_; }
    Decimal avgWin()            const { return avg_win_; }
    Decimal avgLoss()           const { return avg_loss_; }
    size_t   totalTrades()      const { return trades_.size(); }
    size_t   winningTrades()    const { return winning_trades_; }
    size_t   losingTrades()     const { return losing_trades_; }

    // Curves
    const std::vector<std::pair<uint64_t, double>>& equityCurve() const {
        return equity_curve_;
    }
    const std::vector<std::pair<uint64_t, double>>& drawdownCurve() const {
        return drawdown_curve_;
    }

    // --- Export ---

    nlohmann::json exportToJSON() const;
    std::string     exportToCSV() const;

private:
    std::vector<Trade> trades_;
    std::vector<std::pair<uint64_t, Decimal>> equity_snaps_;  // raw input

    // Computed metrics
    Decimal total_return_{0};
    double  annualized_return_{0.0};
    double  sharpe_ratio_{0.0};
    double  sortino_ratio_{0.0};
    double  calmar_ratio_{0.0};

    Decimal max_drawdown_{0};
    double  max_drawdown_pct_{0.0};
    uint64_t max_dd_duration_us_{0};

    double  win_rate_{0.0};
    double  profit_factor_{0.0};
    Decimal avg_win_{0};
    Decimal avg_loss_{0};
    size_t  winning_trades_{0};
    size_t  losing_trades_{0};

    // Derived curves (double for charting convenience)
    std::vector<std::pair<uint64_t, double>> equity_curve_;
    std::vector<std::pair<uint64_t, double>> drawdown_curve_;
};

}  // namespace backtest
}  // namespace chronos
