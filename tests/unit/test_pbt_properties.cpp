/**
 * @file test_pbt_properties.cpp
 * @brief Property-based tests validating correctness invariants via RapidCheck
 *
 * Validates Properties 2, 3, 5, 6, 7, 8, 9, 10, 12, 13 from design.md.
 * Each test is tagged with its property number for traceability.
 */

#include <gtest/gtest.h>

#include <rapidcheck.h>
#include <rapidcheck/gtest.h>
#include <chronos/core/types.hpp>
#include <chronos/market_data/orderbook.hpp>
#include <chronos/market_data/orderbook_v2.hpp>
#include <chronos/trading/order_id_generator.hpp>
#include <chronos/trading/position_manager.hpp>
#include <chronos/risk/risk_engine.hpp>

#include <set>
#include <thread>
#include <vector>

using namespace chronos;
using namespace chronos::market_data;
using namespace chronos::trading;
using namespace chronos::risk;

namespace {

Tick makeTick(uint32_t symbol_id, int64_t price_raw, int64_t qty_raw, TickSide side) {
    Tick t;
    t.symbol_id = symbol_id;
    t.price = Decimal::from_raw_value(price_raw);
    t.quantity = Decimal::from_raw_value(qty_raw);
    t.side = side;
    t.receive_timestamp_us = 1000000;
    return t;
}

/// Fixed-point scale factor: 2^32 (1.0 in Decimal).
constexpr int64_t FXD_SCALE = 1LL << 32;

/// Generate a "price-like" Decimal in the $1–$200k range.
/// Raw value = dollars * 2^32, well within int64_t range.
Decimal genPrice() {
    int64_t dollars = *rc::gen::inRange<int64_t>(1, 200000);
    return Decimal::from_raw_value(dollars * FXD_SCALE);
}

/// Generate a "quantity-like" Decimal in the ~0.004–10 range.
/// Raw value = thousandths * 2^32 / 1000.
Decimal genQty() {
    int64_t thousandths = *rc::gen::inRange<int64_t>(4, 10000);
    return Decimal::from_raw_value(thousandths * (FXD_SCALE / 1000));
}

/// Create a Fill from properly-scaled Decimal values.
Fill makeFill(uint32_t symbol_id, Decimal price, Decimal qty, OrderSide side) {
    Fill f;
    f.symbol_id = symbol_id;
    f.fill_price = price;
    f.fill_quantity = qty;
    f.side = side;
    f.receive_timestamp_us = 1000000;
    return f;
}

/// Configure a RiskEngine with permissive defaults and ample capital.
void configurePermissive(RiskEngine& engine) {
    RiskParameters params;
    params.max_order_value         = 1e9;
    params.max_orders_per_second   = 10000000;
    params.max_position_value      = 1e9;
    params.max_total_position_value = 1e9;
    params.min_available_capital   = 0;
    engine.updateParameters(params);
    engine.setAvailableCapital(toDecimal(1e12));
}

} // anonymous namespace

// ============================================================================
// Property 2: OrderBook Best Bid Invariant
// Feature: chronos, Property 2: For any orderbook state with ≥1 bid,
// getBestBid() SHALL return the price level with the highest bid price.
// ============================================================================

RC_GTEST_PROP(OrderBookProperties_P2, BestBidIsMaxPrice_V1, ())
{
    OrderBook book;
    int num_updates = *rc::gen::inRange(1, 40);
    int64_t max_price_raw = 0;

    for (int i = 0; i < num_updates; ++i) {
        int64_t price_raw = *rc::gen::inRange<int64_t>(1, 1000000);
        int64_t qty_raw  = *rc::gen::inRange<int64_t>(1, 100000);
        book.update(makeTick(1, price_raw, qty_raw, TickSide::BID));
        if (price_raw > max_price_raw) max_price_raw = price_raw;
    }

    auto best = book.getBestBid();
    RC_ASSERT(best.has_value());
    RC_ASSERT(best.value() == Decimal::from_raw_value(max_price_raw));
}

RC_GTEST_PROP(OrderBookProperties_P2, BestBidIsMaxPrice_V2, ())
{
    OrderBookV2 book;
    int num_updates = *rc::gen::inRange(1, 40);
    int64_t max_price_raw = 0;

    for (int i = 0; i < num_updates; ++i) {
        int64_t price_raw = *rc::gen::inRange<int64_t>(1, 1000000);
        int64_t qty_raw  = *rc::gen::inRange<int64_t>(1, 100000);
        book.update(makeTick(1, price_raw, qty_raw, TickSide::BID));
        if (price_raw > max_price_raw) max_price_raw = price_raw;
    }

    auto best = book.getBestBid();
    RC_ASSERT(best.has_value());
    RC_ASSERT(best.value() == Decimal::from_raw_value(max_price_raw));
}

// ============================================================================
// Property 3: OrderBook Best Ask Invariant
// Feature: chronos, Property 3: For any orderbook state with ≥1 ask,
// getBestAsk() SHALL return the price level with the lowest ask price.
// ============================================================================

RC_GTEST_PROP(OrderBookProperties_P3, BestAskIsMinPrice_V1, ())
{
    OrderBook book;
    int num_updates = *rc::gen::inRange(1, 40);
    int64_t min_price_raw = INT64_MAX;

    for (int i = 0; i < num_updates; ++i) {
        int64_t price_raw = *rc::gen::inRange<int64_t>(1, 1000000);
        int64_t qty_raw  = *rc::gen::inRange<int64_t>(1, 100000);
        book.update(makeTick(1, price_raw, qty_raw, TickSide::ASK));
        if (price_raw < min_price_raw) min_price_raw = price_raw;
    }

    auto best = book.getBestAsk();
    RC_ASSERT(best.has_value());
    RC_ASSERT(best.value() == Decimal::from_raw_value(min_price_raw));
}

RC_GTEST_PROP(OrderBookProperties_P3, BestAskIsMinPrice_V2, ())
{
    OrderBookV2 book;
    int num_updates = *rc::gen::inRange(1, 40);
    int64_t min_price_raw = INT64_MAX;

    for (int i = 0; i < num_updates; ++i) {
        int64_t price_raw = *rc::gen::inRange<int64_t>(1, 1000000);
        int64_t qty_raw  = *rc::gen::inRange<int64_t>(1, 100000);
        book.update(makeTick(1, price_raw, qty_raw, TickSide::ASK));
        if (price_raw < min_price_raw) min_price_raw = price_raw;
    }

    auto best = book.getBestAsk();
    RC_ASSERT(best.has_value());
    RC_ASSERT(best.value() == Decimal::from_raw_value(min_price_raw));
}

// ============================================================================
// Property 5: Order ID Uniqueness
// Feature: chronos, Property 5: For any number of concurrent order ID generation
// operations, all generated IDs SHALL be globally unique.
// ============================================================================

RC_GTEST_PROP(OrderIDProperties_P5, UniquenessAcrossThreads, ())
{
    int num_threads     = *rc::gen::inRange(2, 8);
    int ids_per_thread  = *rc::gen::inRange(100, 500);

    OrderIDGenerator gen;
    std::vector<std::thread> threads;
    std::vector<std::vector<uint64_t>> thread_ids(num_threads);

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&gen, &thread_ids, i, ids_per_thread]() {
            for (int j = 0; j < ids_per_thread; ++j) {
                thread_ids[i].push_back(gen.generate());
            }
        });
    }
    for (auto& t : threads) t.join();

    std::set<uint64_t> all_ids;
    for (const auto& ids : thread_ids) {
        all_ids.insert(ids.begin(), ids.end());
    }

    RC_ASSERT(all_ids.size() == static_cast<size_t>(num_threads * ids_per_thread));
}

// ============================================================================
// Property 6: Risk Check — Order Value Limit
// Feature: chronos, Property 6: If order value exceeds max_order_value, the
// risk check SHALL fail with a non-empty rejection reason.
// ============================================================================

RC_GTEST_PROP(RiskProperties_P6, OrderValueExceedsLimit, ())
{
    PositionManager pm;
    RiskEngine engine(pm);
    configurePermissive(engine);

    Decimal max_ov = genPrice();
    Decimal price  = genPrice();
    Decimal qty    = genQty();

    RiskParameters params;
    params.max_order_value        = toDouble(max_ov);
    params.max_orders_per_second  = 10000000;
    params.max_position_value     = 1e9;
    params.max_total_position_value = 1e9;
    params.min_available_capital  = 0;
    engine.updateParameters(params);

    OrderRequest order;
    order.symbol_id = 1;
    order.price     = price;
    order.quantity  = qty;
    order.side      = OrderSide::BUY;
    order.type      = OrderType::LIMIT;

    auto result = engine.checkOrder(order);
    Decimal order_value = price * qty;

    RC_CLASSIFY(order_value > max_ov, "exceeds");
    RC_CLASSIFY(order_value <= max_ov, "within");

    if (order_value > max_ov) {
        RC_ASSERT(!result.passed);
        RC_ASSERT(!result.rejection_reason.empty());
    }
}

// ============================================================================
// Property 7: Risk Check — Position Limit
// Feature: chronos, Property 7: If a new order would cause the symbol position
// value to exceed max_position_value, the risk check SHALL fail.
// ============================================================================

RC_GTEST_PROP(RiskProperties_P7, PositionValueExceedsLimit, ())
{
    PositionManager pm;
    RiskEngine engine(pm);
    configurePermissive(engine);

    Decimal max_pos = genPrice();
    Decimal price   = genPrice();
    Decimal existing_qty = *rc::gen::inRange(0, 1) ? genQty() : Decimal(0);

    RiskParameters params;
    params.max_order_value        = 1e9;
    params.max_orders_per_second  = 10000000;
    params.max_position_value     = toDouble(max_pos);
    params.max_total_position_value = 1e9;
    params.min_available_capital  = 0;
    engine.updateParameters(params);

    // Pre-populate an existing position
    if (existing_qty > Decimal(0)) {
        Fill f = makeFill(1, price, existing_qty, OrderSide::BUY);
        pm.updatePosition(f);
    }

    Decimal new_qty = genQty();
    OrderRequest order;
    order.symbol_id = 1;
    order.price     = price;
    order.quantity  = new_qty;
    order.side      = OrderSide::BUY;
    order.type      = OrderType::LIMIT;

    auto result = engine.checkOrder(order);

    // Projected position value
    Decimal projected_abs = (existing_qty + new_qty);
    Decimal projected_val = projected_abs * price;
    Decimal pos_limit     = max_pos;

    RC_CLASSIFY(projected_val > pos_limit, "exceeds");
    RC_CLASSIFY(projected_val <= pos_limit, "within");

    if (projected_val > pos_limit) {
        RC_ASSERT(!result.passed);
    }
}

// ============================================================================
// Property 8: Risk Check — Rate Limiting
// Feature: chronos, Property 8: After exceeding max_orders_per_second within
// the same second window, subsequent orders SHALL be rejected.
// ============================================================================

RC_GTEST_PROP(RiskProperties_P8, RateLimitingEnforced, ())
{
    PositionManager pm;
    RiskEngine engine(pm);
    configurePermissive(engine);

    uint32_t max_rate = *rc::gen::inRange<uint32_t>(5, 50);
    uint32_t burst    = *rc::gen::inRange<uint32_t>(max_rate + 1, max_rate + 100);

    RiskParameters params;
    params.max_order_value        = 1e9;
    params.max_orders_per_second  = max_rate;
    params.max_position_value     = 1e9;
    params.max_total_position_value = 1e9;
    params.min_available_capital  = 0;
    engine.updateParameters(params);

    OrderRequest order;
    order.symbol_id = 1;
    order.price     = toDecimal(100.0);
    order.quantity  = toDecimal(0.01);
    order.side      = OrderSide::BUY;
    order.type      = OrderType::LIMIT;

    // Fire orders within the same second window
    int rejected = 0;
    for (uint32_t i = 0; i < burst; ++i) {
        auto result = engine.checkOrder(order);
        if (!result.passed) {
            ++rejected;
            RC_ASSERT(!result.rejection_reason.empty());
        }
    }

    // If burst exceeds rate limit, at least some orders must be rejected
    RC_ASSERT(rejected > 0 || burst <= max_rate);
}

// ============================================================================
// Property 9: Risk Check — Capital Sufficiency
// Feature: chronos, Property 9: If an order requires more capital than
// available, the risk check SHALL fail.
// ============================================================================

RC_GTEST_PROP(RiskProperties_P9, CapitalInsufficiencyRejected, ())
{
    PositionManager pm;
    RiskEngine engine(pm);
    configurePermissive(engine);

    Decimal capital = genPrice();
    Decimal price   = genPrice();
    Decimal qty     = genQty();

    engine.setAvailableCapital(capital);

    OrderRequest order;
    order.symbol_id = 1;
    order.price     = price;
    order.quantity  = qty;
    order.side      = OrderSide::BUY;
    order.type      = OrderType::LIMIT;

    auto result = engine.checkOrder(order);
    Decimal order_value = price * qty;

    RC_CLASSIFY(order_value > capital, "insufficient");
    RC_CLASSIFY(order_value <= capital, "sufficient");

    if (order_value > capital) {
        RC_ASSERT(!result.passed);
        RC_ASSERT(!result.rejection_reason.empty());
    }
}

// ============================================================================
// Property 10: Risk Check — Rejection Reason
// Feature: chronos, Property 10: For any failed risk check, the RiskCheckResult
// SHALL contain a non-empty rejection reason string.
// ============================================================================

RC_GTEST_PROP(RiskProperties_P10, FailedCheckHasReason, ())
{
    PositionManager pm;
    RiskEngine engine(pm);
    configurePermissive(engine);

    // Set very tight params to trigger rejection
    RiskParameters tight;
    tight.max_order_value           = 1.0;
    tight.max_orders_per_second     = 0;
    tight.max_position_value        = 1.0;
    tight.max_total_position_value  = 1.0;
    tight.min_available_capital     = 1e9;
    engine.updateParameters(tight);
    engine.setAvailableCapital(Decimal(0));

    Decimal price = genPrice();
    Decimal qty   = genQty();

    // Pre-populate position to trigger position limits
    Fill f = makeFill(1, price, qty, OrderSide::BUY);
    pm.updatePosition(f);

    OrderRequest order;
    order.symbol_id = 1;
    order.price     = price;
    order.quantity  = qty;
    order.side      = OrderSide::BUY;
    order.type      = OrderType::LIMIT;

    auto result = engine.checkOrder(order);
    RC_ASSERT(!result.passed);
    RC_ASSERT(!result.rejection_reason.empty());
}

// ============================================================================
// Property 12: Position Average Price — Weighted Average
// Feature: chronos, Property 12: For any sequence of fills, the average price
// SHALL equal the quantity-weighted average of all fill prices.
// ============================================================================

RC_GTEST_PROP(PositionProperties_P12, WeightedAveragePrice, ())
{
    PositionManager pm;
    int num_fills = *rc::gen::inRange(1, 30);

    Decimal total_qty(0);
    Decimal weighted_sum(0);

    for (int i = 0; i < num_fills; ++i) {
        Decimal price = genPrice();
        Decimal qty   = genQty();
        Fill f = makeFill(1, price, qty, OrderSide::BUY);
        pm.updatePosition(f);

        total_qty = total_qty + qty;
        weighted_sum = weighted_sum + (price * qty);
    }

    const Position* pos = pm.getPosition(1);
    RC_ASSERT(pos != nullptr);

    if (total_qty > Decimal(0)) {
        Decimal expected_avg = weighted_sum / total_qty;
        // Allow small tolerance for fixed-point rounding
        int64_t diff = (pos->average_price - expected_avg).raw_value();
        RC_ASSERT(diff >= -10 && diff <= 10);
    }
}

// ============================================================================
// Property 13: Position Unrealized P&L Formula
// Feature: chronos, Property 13: For any position and current market price,
// unrealized P&L SHALL equal quantity * (current_price - average_price).
// ============================================================================

RC_GTEST_PROP(PositionProperties_P13, UnrealizedPnLFormula, ())
{
    PositionManager pm;

    Decimal entry_price = genPrice();
    Decimal entry_qty   = genQty();
    Decimal current_price = genPrice();

    // Open a long position
    Fill f = makeFill(1, entry_price, entry_qty, OrderSide::BUY);
    pm.updatePosition(f);

    const Position* pos = pm.getPosition(1);
    RC_ASSERT(pos != nullptr);

    Decimal expected_pnl = pos->quantity * (current_price - pos->average_price);
    Decimal actual_pnl   = pos->getUnrealizedPnL(current_price);

    RC_ASSERT(expected_pnl == actual_pnl);
}
