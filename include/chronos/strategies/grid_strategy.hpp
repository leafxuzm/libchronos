#pragma once

#include "chronos/trading/strategy.hpp"
#include <cmath>
#include <vector>
#include <unordered_map>
#include <string>
#include <cstdint>

namespace chronos {
namespace strategies {

// ============================================================================
// GridStrategy — classic grid trading example
// ============================================================================
//
// Places buy/sell limit orders at evenly spaced price levels within [low, high].
// When a buy fills, a take-profit sell is placed one level above.
// When a sell fills, a buy is placed one level below.
//
// This demonstrates the full Strategy lifecycle:
//   onLoad → onTick → submitOrder → onFill → onTimer → onUnload

class GridStrategy : public trading::Strategy {
public:
    struct Config {
        double grid_low   = 90.0;
        double grid_high  = 110.0;
        int    grid_levels = 10;
        double quantity   = 0.01;
        uint32_t symbol_id = 1;
    };

    explicit GridStrategy(const Config& cfg) : cfg_(cfg) {
        double spacing = (cfg_.grid_high - cfg_.grid_low) / cfg_.grid_levels;
        levels_.reserve(static_cast<size_t>(cfg_.grid_levels + 1));
        for (int i = 0; i <= cfg_.grid_levels; ++i) {
            levels_.push_back({toDecimal(cfg_.grid_low + spacing * i), 0, 0});
        }
    }

    const char* name() const override { return "GridStrategy"; }

    std::vector<uint32_t> symbols() const override { return {cfg_.symbol_id}; }

    void onLoad(trading::StrategyContext& /*ctx*/) override {
        initialized_ = true;
    }

    void onUnload(trading::StrategyContext& /*ctx*/) override {
        initialized_ = false;
    }

    void onTick(const Tick& /*tick*/, trading::StrategyContext& ctx) override {
        if (!initialized_) return;

        auto mid = ctx.getMidPrice(cfg_.symbol_id);
        if (!mid) return;

        double mid_val = toDouble(*mid);
        int buy_offset = 0, sell_offset = 0;
        for (size_t i = 0; i < levels_.size(); ++i) {
            double level_price = toDouble(levels_[i].price);

            if (level_price < mid_val && levels_[i].buy_order_id == 0) {
                // Cross spread by 5 ticks (0.5) to ensure fill on thin testnet
                double aggressive = std::max(level_price, mid_val) + 0.5 + buy_offset * 0.1;
                buy_offset++;
                auto oid = placeBuyAt(i, toDecimal(aggressive), ctx);
                if (oid) ticks_processed_++;
            }
            if (level_price > mid_val && levels_[i].sell_order_id == 0) {
                double aggressive = std::min(level_price, mid_val) - 0.5 - sell_offset * 0.1;
                sell_offset++;
                auto oid = placeSellAt(i, toDecimal(aggressive), ctx);
                if (oid) ticks_processed_++;
            }
        }
    }

    void onFill(const Fill& fill, trading::StrategyContext& ctx) override {
        auto mid = ctx.getMidPrice(cfg_.symbol_id);
        double mid_val = mid ? toDouble(*mid) : 0.0;

        for (size_t i = 0; i < levels_.size(); ++i) {
            if (fill.order_id != 0 && fill.order_id == levels_[i].buy_order_id) {
                levels_[i].buy_order_id = 0;
                fills_processed_++;
                if (i + 1 < levels_.size()) {
                    double price = toDouble(levels_[i + 1].price);
                    if (mid_val > 0) price = std::min(price, mid_val);
                    placeSellAt(i + 1, toDecimal(price), ctx);
                }
                return;
            }
            if (fill.order_id != 0 && fill.order_id == levels_[i].sell_order_id) {
                levels_[i].sell_order_id = 0;
                fills_processed_++;
                if (i > 0) {
                    double price = toDouble(levels_[i - 1].price);
                    if (mid_val > 0) price = std::max(price, mid_val);
                    placeBuyAt(i - 1, toDecimal(price), ctx);
                }
                return;
            }
        }
    }

    void onTimer(uint64_t /*ts*/, trading::StrategyContext& ctx) override {
        if (!initialized_) return;

        auto mid = ctx.getMidPrice(cfg_.symbol_id);
        if (!mid) return;

        double mid_val = toDouble(*mid);

        // Cancel orders that are on the wrong side of mid (stale)
        for (size_t i = 0; i < levels_.size(); ++i) {
            double price = toDouble(levels_[i].price);

            // Cancel buy above mid
            if (price > mid_val && levels_[i].buy_order_id != 0) {
                ctx.cancelOrder(levels_[i].buy_order_id);
                levels_[i].buy_order_id = 0;
            }
            // Cancel sell below mid
            if (price < mid_val && levels_[i].sell_order_id != 0) {
                ctx.cancelOrder(levels_[i].sell_order_id);
                levels_[i].sell_order_id = 0;
            }
        }
    }

    // --- State accessors for testing ---
    const Config& config() const { return cfg_; }
    uint64_t tickCount()    const { return ticks_processed_.load(); }
    uint64_t fillCount()    const { return fills_processed_.load(); }
    bool     isInitialized() const { return initialized_; }

    struct GridLevel {
        Decimal price;
        uint64_t buy_order_id;
        uint64_t sell_order_id;
    };
    const std::vector<GridLevel>& levels() const { return levels_; }

private:
    uint64_t placeBuy(size_t level_idx, trading::StrategyContext& ctx) {
        return placeBuyAt(level_idx, levels_[level_idx].price, ctx);
    }

    uint64_t placeBuyAt(size_t level_idx, Decimal price, trading::StrategyContext& ctx) {
        OrderRequest order;
        order.symbol_id   = cfg_.symbol_id;
        order.price       = roundPrice(price, 1);  // tick size 0.1 for BTCUSDT
        order.quantity    = toDecimal(cfg_.quantity);
        order.side        = OrderSide::BUY;
        order.type        = OrderType::LIMIT;
        order.tif         = TimeInForce::GTC;

        uint64_t oid = ctx.submitOrder(order);
        if (oid) {
            levels_[level_idx].buy_order_id = oid;
        }
        return oid;
    }

    uint64_t placeSell(size_t level_idx, trading::StrategyContext& ctx) {
        return placeSellAt(level_idx, levels_[level_idx].price, ctx);
    }

    uint64_t placeSellAt(size_t level_idx, Decimal price, trading::StrategyContext& ctx) {
        OrderRequest order;
        order.symbol_id   = cfg_.symbol_id;
        order.price       = roundPrice(price, 1);  // tick size 0.1 for BTCUSDT
        order.quantity    = toDecimal(cfg_.quantity);
        order.side        = OrderSide::SELL;
        order.type        = OrderType::LIMIT;
        order.tif         = TimeInForce::GTC;

        uint64_t oid = ctx.submitOrder(order);
        if (oid) {
            levels_[level_idx].sell_order_id = oid;
        }
        return oid;
    }

    Config cfg_;
    std::vector<GridLevel> levels_;
    bool initialized_{false};

    std::atomic<uint64_t> ticks_processed_{0};
    std::atomic<uint64_t> fills_processed_{0};

    static Decimal roundPrice(Decimal price, int decimals) {
        double v = toDouble(price);
        double scale = std::pow(10.0, decimals);
        return toDecimal(std::round(v * scale) / scale);
    }
};

}  // namespace strategies
}  // namespace chronos
