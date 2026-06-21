#include "chronos/backtest/metrics.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <sstream>

namespace chronos {
namespace backtest {

namespace {

constexpr double TRADING_DAYS_PER_YEAR = 365.0;  // crypto trades 365 days

inline double toD(Decimal d) { return toDouble(d); }

// Daily returns from equity curve (1-day buckets on UTC day boundaries)
std::vector<double> dailyReturns(const std::vector<std::pair<uint64_t, double>>& equity) {
    if (equity.size() < 2) return {};

    constexpr uint64_t DAY_US = 86'400'000'000ULL;
    std::vector<double> rets;

    // Group by day, take last equity value each day
    uint64_t day_start = (equity[0].first / DAY_US) * DAY_US;
    size_t i = 0;
    double prev_day_close = equity[0].second;

    while (i < equity.size()) {
        // Find last snap of this day
        size_t j = i;
        while (j + 1 < equity.size() && equity[j + 1].first < day_start + DAY_US) {
            j++;
        }

        double close = equity[j].second;
        if (day_start > (equity[0].first / DAY_US) * DAY_US) {
            double ret = (close - prev_day_close) / prev_day_close;
            rets.push_back(ret);
        }
        prev_day_close = close;

        // Next day
        day_start += DAY_US;
        while (i < equity.size() && equity[i].first < day_start) i++;
        if (i >= equity.size()) break;
    }

    return rets;
}

double mean(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    return std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
}

double stddev(const std::vector<double>& v, double m) {
    if (v.size() < 2) return 0.0;
    double sq = 0.0;
    for (double x : v) sq += (x - m) * (x - m);
    return std::sqrt(sq / static_cast<double>(v.size() - 1));
}

}  // namespace

// ============================================================================
// Data Input
// ============================================================================

void MetricsCollector::recordTrade(const Trade& trade) {
    trades_.push_back(trade);
}

void MetricsCollector::recordEquity(uint64_t timestamp_us, Decimal equity) {
    equity_snaps_.emplace_back(timestamp_us, equity);
}

// ============================================================================
// Calculate Metrics
// ============================================================================

void MetricsCollector::calculateMetrics() {
    // --- Build equity curve (double for charting) ---
    equity_curve_.clear();
    equity_curve_.reserve(equity_snaps_.size());
    for (auto& [ts, eq] : equity_snaps_) {
        equity_curve_.emplace_back(ts, toD(eq));
    }

    // --- Drawdown ---
    max_drawdown_ = Decimal(0);
    max_drawdown_pct_ = 0.0;
    max_dd_duration_us_ = 0;
    drawdown_curve_.clear();

    if (!equity_snaps_.empty()) {
        drawdown_curve_.reserve(equity_snaps_.size());
        Decimal peak = equity_snaps_[0].second;
        uint64_t dd_start = 0;
        bool in_dd = false;

        for (auto& [ts, eq] : equity_snaps_) {
            if (eq > peak) {
                peak = eq;
                if (in_dd) {
                    uint64_t dur = ts - dd_start;
                    if (dur > max_dd_duration_us_) max_dd_duration_us_ = dur;
                    in_dd = false;
                }
            }

            Decimal dd = peak - eq;
            double dd_pct = peak > Decimal(0) ? toD(dd) / toD(peak) * 100.0 : 0.0;
            drawdown_curve_.emplace_back(ts, dd_pct);

            if (dd > max_drawdown_) {
                max_drawdown_ = dd;
                max_drawdown_pct_ = dd_pct;
            }

            if (dd > Decimal(0) && !in_dd) {
                dd_start = ts;
                in_dd = true;
            }
        }

        // Close out final drawdown if still in one
        if (in_dd && !equity_snaps_.empty()) {
            uint64_t dur = equity_snaps_.back().first - dd_start;
            if (dur > max_dd_duration_us_) max_dd_duration_us_ = dur;
        }
    }

    // --- Total Return ---
    if (equity_snaps_.size() >= 2) {
        Decimal initial = equity_snaps_.front().second;
        Decimal final_eq = equity_snaps_.back().second;
        if (initial > Decimal(0)) {
            total_return_ = (final_eq - initial) / initial;
        }
    }

    // --- Annualized Return ---
    annualized_return_ = 0.0;
    if (equity_snaps_.size() >= 2) {
        uint64_t first_ts = equity_snaps_.front().first;
        uint64_t last_ts  = equity_snaps_.back().first;
        if (last_ts > first_ts) {
            double years = static_cast<double>(last_ts - first_ts)
                         / (86400.0 * 1e6 * 365.0);
            double tr = toD(total_return_);
            if (years > 0.001 && tr > -1.0) {
                annualized_return_ = std::pow(1.0 + tr, 1.0 / years) - 1.0;
            }
        }
    }

    // --- Daily returns ---
    auto daily_rets = dailyReturns(equity_curve_);
    double avg_daily = mean(daily_rets);
    double std_daily = stddev(daily_rets, avg_daily);

    // --- Sharpe Ratio ---
    if (std_daily > 0.0 && !daily_rets.empty()) {
        // Annualized: daily Sharpe * sqrt(365)
        sharpe_ratio_ = (avg_daily / std_daily) * std::sqrt(TRADING_DAYS_PER_YEAR);
    }

    // --- Sortino Ratio ---
    {
        std::vector<double> downside;
        for (double r : daily_rets) {
            if (r < 0.0) downside.push_back(r);
        }
        if (!downside.empty()) {
            double avg_down = mean(downside);
            double std_down = stddev(downside, avg_down);
            if (std_down > 0.0) {
                sortino_ratio_ = (avg_daily / std_down) * std::sqrt(TRADING_DAYS_PER_YEAR);
            }
        }
    }

    // --- Calmar Ratio ---
    if (max_drawdown_pct_ > 0.0) {
        calmar_ratio_ = annualized_return_ / (max_drawdown_pct_ / 100.0);
    }

    // --- Win Rate / Profit Factor ---
    Decimal total_gross_win{0}, total_gross_loss{0};
    winning_trades_ = 0;
    losing_trades_ = 0;

    for (auto& t : trades_) {
        if (t.pnl > Decimal(0)) {
            winning_trades_++;
            total_gross_win += t.pnl;
        } else if (t.pnl < Decimal(0)) {
            losing_trades_++;
            total_gross_loss += t.pnl;  // negative
        }
    }

    if (trades_.size() > 0) {
        win_rate_ = static_cast<double>(winning_trades_) / static_cast<double>(trades_.size());
    }

    if (total_gross_loss < Decimal(0)) {
        profit_factor_ = toD(total_gross_win) / std::abs(toD(total_gross_loss));
    } else if (total_gross_win > Decimal(0)) {
        profit_factor_ = std::numeric_limits<double>::infinity();
    }

    if (winning_trades_ > 0) avg_win_ = total_gross_win / static_cast<int64_t>(winning_trades_);
    if (losing_trades_ > 0) avg_loss_ = total_gross_loss / static_cast<int64_t>(losing_trades_);
}

// ============================================================================
// Export
// ============================================================================

nlohmann::json MetricsCollector::exportToJSON() const {
    nlohmann::json j;
    j["total_return"] = toD(total_return_);
    j["annualized_return"] = annualized_return_;
    j["sharpe_ratio"] = sharpe_ratio_;
    j["sortino_ratio"] = sortino_ratio_;
    j["calmar_ratio"] = calmar_ratio_;
    j["max_drawdown"] = toD(max_drawdown_);
    j["max_drawdown_pct"] = max_drawdown_pct_;
    j["max_drawdown_duration_us"] = max_dd_duration_us_;
    j["win_rate"] = win_rate_;
    j["profit_factor"] = profit_factor_;
    j["avg_win"] = toD(avg_win_);
    j["avg_loss"] = toD(avg_loss_);
    j["total_trades"] = trades_.size();
    j["winning_trades"] = winning_trades_;
    j["losing_trades"] = losing_trades_;

    // Equity curve
    auto& eq = j["equity_curve"] = nlohmann::json::array();
    for (auto& [ts, val] : equity_curve_) eq.push_back({ts, val});

    // Drawdown curve
    auto& dd = j["drawdown_curve"] = nlohmann::json::array();
    for (auto& [ts, pct] : drawdown_curve_) dd.push_back({ts, pct});

    return j;
}

std::string MetricsCollector::exportToCSV() const {
    std::ostringstream oss;

    // Metrics header
    oss << "metric,value\n";
    oss << "total_return," << toD(total_return_) << "\n";
    oss << "annualized_return," << annualized_return_ << "\n";
    oss << "sharpe_ratio," << sharpe_ratio_ << "\n";
    oss << "sortino_ratio," << sortino_ratio_ << "\n";
    oss << "calmar_ratio," << calmar_ratio_ << "\n";
    oss << "max_drawdown," << toD(max_drawdown_) << "\n";
    oss << "max_drawdown_pct," << max_drawdown_pct_ << "\n";
    oss << "win_rate," << win_rate_ << "\n";
    oss << "profit_factor," << profit_factor_ << "\n";
    oss << "avg_win," << toD(avg_win_) << "\n";
    oss << "avg_loss," << toD(avg_loss_) << "\n";
    oss << "total_trades," << trades_.size() << "\n";
    oss << "winning_trades," << winning_trades_ << "\n";
    oss << "losing_trades," << losing_trades_ << "\n";

    // Equity curve
    oss << "\ntimestamp_us,equity\n";
    for (auto& [ts, val] : equity_curve_) oss << ts << "," << val << "\n";

    // Drawdown curve
    oss << "\ntimestamp_us,drawdown_pct\n";
    for (auto& [ts, pct] : drawdown_curve_) oss << ts << "," << pct << "\n";

    return oss.str();
}

}  // namespace backtest
}  // namespace chronos
