#include "chronos/trading/position_manager.hpp"
#include "chronos/core/error.hpp"
#include <cmath>
#include <fstream>

namespace chronos {
namespace trading {

// ============================================================================
// Helpers
// ============================================================================

namespace {

/// Sign of a Decimal value: -1, 0, or 1.
inline int sign_of(Decimal v) {
    if (v > Decimal(0)) return 1;
    if (v < Decimal(0)) return -1;
    return 0;
}

}  // anonymous namespace

Decimal PositionManager::calcWeightedAverage(
    Decimal old_qty, Decimal old_avg,
    Decimal fill_qty, Decimal fill_price)
{
    Decimal old_abs = old_qty < Decimal(0) ? -old_qty : old_qty;
    Decimal new_abs = old_abs + fill_qty;
    if (new_abs == Decimal(0)) return Decimal(0);
    return (old_abs * old_avg + fill_qty * fill_price) / new_abs;
}

Decimal PositionManager::calcRealizedPnLOnReduce(
    const Position& pos, Decimal fill_price, Decimal reduced_abs)
{
    if (pos.quantity > Decimal(0)) {
        // Long: selling at fill_price, entered at average_price
        return reduced_abs * (fill_price - pos.average_price);
    } else {
        // Short: buying back at fill_price, shorted at average_price
        return reduced_abs * (pos.average_price - fill_price);
    }
}

// ============================================================================
// updatePosition — the core mutation
// ============================================================================

const Position* PositionManager::updatePosition(const Fill& fill) {
    // Guard against zero-quantity fills
    if (fill.fill_quantity == Decimal(0)) {
        return nullptr;
    }

    // Convert fill side to a signed quantity delta
    Decimal signed_qty = fill.fill_quantity;
    if (fill.side == OrderSide::SELL) {
        signed_qty = -signed_qty;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = positions_.find(fill.symbol_id);
    if (it == positions_.end()) {
        // No existing position — create one
        Position p;
        p.symbol_id = fill.symbol_id;
        p.quantity = signed_qty;
        p.average_price = fill.fill_price;
        p.realized_pnl = Decimal(0);
        p.last_update_us = fill.receive_timestamp_us;
        positions_[fill.symbol_id] = p;
        recalcTotalValue();
        return &positions_[fill.symbol_id];
    }

    Position& pos = it->second;
    Decimal old_qty = pos.quantity;
    int old_sign = sign_of(old_qty);
    int fill_sign = sign_of(signed_qty);

    if (old_sign == 0) {
        // Flat position — re-establish
        pos.quantity = signed_qty;
        pos.average_price = fill.fill_price;
        // realized_pnl unchanged
        pos.last_update_us = fill.receive_timestamp_us;
        recalcTotalValue();
        return &pos;
    }

    if (old_sign == fill_sign) {
        // Same direction — increase position
        pos.quantity = old_qty + signed_qty;
        pos.average_price = calcWeightedAverage(
            old_qty, pos.average_price,
            fill.fill_quantity, fill.fill_price);
        // realized_pnl unchanged
        pos.last_update_us = fill.receive_timestamp_us;
        recalcTotalValue();
        return &pos;
    }

    // Opposite direction — reduction or flip
    Decimal old_abs = old_qty < Decimal(0) ? -old_qty : old_qty;
    Decimal signed_abs = signed_qty < Decimal(0) ? -signed_qty : signed_qty;

    if (signed_abs <= old_abs) {
        // Partial or full reduction — average price unchanged
        pos.realized_pnl = pos.realized_pnl +
            calcRealizedPnLOnReduce(pos, fill.fill_price, signed_abs);
        pos.quantity = old_qty + signed_qty;
        if (pos.quantity == Decimal(0)) {
            pos.average_price = Decimal(0);
        }
        // else: average_price unchanged for partial reduction
        pos.last_update_us = fill.receive_timestamp_us;
        recalcTotalValue();
        return &pos;
    } else {
        // Flip: close old position entirely, open new on opposite side
        pos.realized_pnl = pos.realized_pnl +
            calcRealizedPnLOnReduce(pos, fill.fill_price, old_abs);
        Decimal remaining = signed_abs - old_abs;
        pos.quantity = (fill_sign > 0) ? remaining : -remaining;
        pos.average_price = fill.fill_price;
        pos.last_update_us = fill.receive_timestamp_us;
        recalcTotalValue();
        return &pos;
    }
}

// ============================================================================
// Queries
// ============================================================================

const Position* PositionManager::getPosition(uint32_t symbol_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = positions_.find(symbol_id);
    if (it == positions_.end()) return nullptr;
    return &it->second;
}

size_t PositionManager::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t n = 0;
    for (const auto& [id, pos] : positions_) {
        if (pos.quantity != Decimal(0)) ++n;
    }
    return n;
}

void PositionManager::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    positions_.clear();
    total_value_raw_.store(0, std::memory_order_release);
}

std::vector<Position> PositionManager::getAllPositions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Position> result;
    result.reserve(positions_.size());
    for (const auto& [id, pos] : positions_) {
        result.push_back(pos);
    }
    return result;
}

Decimal PositionManager::getUnrealizedPnL(
    const std::unordered_map<uint32_t, Decimal>& current_prices) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    Decimal total(0);
    for (const auto& [symbol_id, pos] : positions_) {
        auto it = current_prices.find(symbol_id);
        if (it != current_prices.end() && it->second > Decimal(0)) {
            total = total + pos.getUnrealizedPnL(it->second);
        }
    }
    return total;
}

Decimal PositionManager::getTotalValue(
    const std::unordered_map<uint32_t, Decimal>& current_prices) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    Decimal total(0);
    for (const auto& [symbol_id, pos] : positions_) {
        if (pos.quantity == Decimal(0)) continue;
        auto it = current_prices.find(symbol_id);
        Decimal price = (it != current_prices.end() && it->second > Decimal(0))
            ? it->second
            : pos.average_price;
        Decimal abs_qty = pos.quantity < Decimal(0) ? -pos.quantity : pos.quantity;
        total = total + (abs_qty * price);
    }
    return total;
}

void PositionManager::recalcTotalValue() {
    // Called under mutex_
    int64_t raw = 0;
    for (const auto& [id, pos] : positions_) {
        if (pos.quantity == Decimal(0)) continue;
        Decimal abs_qty = pos.quantity < Decimal(0) ? -pos.quantity : pos.quantity;
        Decimal val = abs_qty * pos.average_price;
        // Saturating add to avoid overflow
        int64_t add = val.raw_value();
        if (add > 0 && raw > INT64_MAX - add) {
            raw = INT64_MAX;
        } else if (add < 0 && raw < INT64_MIN - add) {
            raw = INT64_MIN;
        } else {
            raw += add;
        }
    }
    total_value_raw_.store(raw, std::memory_order_release);
}

// ============================================================================
// Persistence
// ============================================================================

nlohmann::json PositionManager::toJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json j = nlohmann::json::array();
    for (const auto& [id, pos] : positions_) {
        if (pos.quantity == Decimal(0)) continue;  // skip flat positions
        nlohmann::json entry;
        entry["symbol_id"] = pos.symbol_id;
        entry["quantity_raw"] = pos.quantity.raw_value();
        entry["average_price_raw"] = pos.average_price.raw_value();
        entry["realized_pnl_raw"] = pos.realized_pnl.raw_value();
        entry["last_update_us"] = pos.last_update_us;
        j.push_back(std::move(entry));
    }
    return j;
}

bool PositionManager::fromJson(const nlohmann::json& j) {
    if (!j.is_array()) return false;

    std::lock_guard<std::mutex> lock(mutex_);
    positions_.clear();

    for (const auto& entry : j) {
        if (!entry.contains("symbol_id") ||
            !entry.contains("quantity_raw") ||
            !entry.contains("average_price_raw") ||
            !entry.contains("realized_pnl_raw")) {
            continue;
        }
        Position p;
        p.symbol_id = entry["symbol_id"].get<uint32_t>();
        p.quantity = Decimal::from_raw_value(entry["quantity_raw"].get<int64_t>());
        p.average_price = Decimal::from_raw_value(
            entry["average_price_raw"].get<int64_t>());
        p.realized_pnl = Decimal::from_raw_value(
            entry["realized_pnl_raw"].get<int64_t>());
        p.last_update_us = entry.value("last_update_us", uint64_t(0));
        positions_[p.symbol_id] = p;
    }

    recalcTotalValue();
    return true;
}

bool PositionManager::savePositions(const std::string& filepath) const {
    try {
        auto j = toJson();
        std::ofstream ofs(filepath);
        if (!ofs) return false;
        ofs << j.dump(2);
        return true;
    } catch (const std::exception& e) {
        CHRONOS_WARNING(ErrorCategory::PERSISTENCE, "PositionManager",
            "Failed to save positions to " + filepath + ": " + e.what());
        return false;
    }
}

bool PositionManager::loadPositions(const std::string& filepath) {
    try {
        std::ifstream ifs(filepath);
        if (!ifs) return false;
        auto j = nlohmann::json::parse(ifs);
        return fromJson(j);
    } catch (const std::exception& e) {
        CHRONOS_WARNING(ErrorCategory::PERSISTENCE, "PositionManager",
            "Failed to load positions from " + filepath + ": " + e.what());
        return false;
    }
}

}  // namespace trading
}  // namespace chronos
