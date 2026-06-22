#pragma once

#include <cstdint>
#include <type_traits>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <chrono>

// GCC: std::is_signed<__int128> incorrectly returns false (known quirk with
// extended integer types). Specialise it BEFORE fpm includes so its
// static_assert(is_signed<IntermediateType> == is_signed<BaseType>) passes.
#ifdef __SIZEOF_INT128__
namespace std {
    template<> struct is_signed<__int128> : true_type {};
}
#endif

#include <fpm/fixed.hpp>  // Fixed-point math library for precise decimal arithmetic

namespace chronos {

// ============================================================================
// Decimal Type for Financial Calculations
// ============================================================================

/**
 * @brief Fixed-point decimal type for precise financial calculations
 * 
 * Uses 64-bit fixed-point arithmetic:
 * - BaseType: int64_t (8 bytes)
 * - IntermediateType: __int128 (16 bytes, for intermediate calculations)
 * - FractionBits: 32 (fractional precision)
 * 
 * Range: ±2,147,483,647 with precision ~2.3e-10
 * Sufficient for cryptocurrency (8 decimals) and traditional financial instruments.
 * 
 * Note: Using 32 fraction bits instead of 48 for better range/precision balance.
 */
using Decimal = fpm::fixed<std::int64_t, __int128, 32>;

/**
 * @brief Fast decimal type for hot-path calculations
 * 
 * Uses 32-bit fixed-point arithmetic with 16.16 format:
 * - BaseType: int32_t (4 bytes)
 * - IntermediateType: int64_t (8 bytes)
 * - FractionBits: 16
 * 
 * Range: ±32,767 with precision ~0.000015
 * Use for price differences, spreads, and other calculations
 * where range and precision requirements are modest.
 */
using DecimalFast = fpm::fixed<std::int32_t, std::int64_t, 16>;

// Helper functions for decimal conversion
// These use fpm's built-in conversion which handles precision correctly

/**
 * @brief Convert double to Decimal
 * 
 * Uses fpm's from_raw_value with proper scaling to avoid precision loss.
 * The scaling factor is 2^32 for our 32-bit fractional part.
 */
inline Decimal toDecimal(double value) {
    // Use fpm's proper conversion: multiply by 2^FractionBits and convert to raw value
    constexpr double scale = static_cast<double>(1ULL << 32);
    return Decimal::from_raw_value(static_cast<std::int64_t>(value * scale));
}

/**
 * @brief Convert Decimal to double
 * 
 * Uses fpm's raw_value() and divides by scaling factor.
 */
inline double toDouble(Decimal value) {
    constexpr double scale = static_cast<double>(1ULL << 32);
    return static_cast<double>(value.raw_value()) / scale;
}

/**
 * @brief Parse a decimal string directly to Decimal fixed-point
 *
 * Parses price/quantity strings like "1234.56", "0.001", "-12.34"
 * directly to Decimal without going through double (no std::stod).
 * Uses __int128 for intermediate computation to avoid overflow.
 *
 * @param sv String view of the number
 * @return Decimal value (returns 0 on parse failure)
 */
Decimal parseDecimal(std::string_view sv);

// ============================================================================
// Enumerations
// ============================================================================

/// Order side: buy or sell
enum class OrderSide : uint8_t {
    BUY = 0,
    SELL = 1
};

/// Exchange identifier
enum class ExchangeId : uint32_t {
    UNKNOWN = 0,
    BINANCE = 1,
    OKX = 2
};

/// Tick side: bid, ask, or trade
enum class TickSide : uint8_t {
    BID = 0,
    ASK = 1,
    TRADE = 2
};

/// Order type
enum class OrderType : uint8_t {
    LIMIT = 0,
    MARKET = 1,
    STOP_LOSS = 2,
    STOP_LIMIT = 3
};

/// Time in force
enum class TimeInForce : uint8_t {
    GTC = 0,  // Good Till Cancel
    IOC = 1,  // Immediate Or Cancel
    FOK = 2,  // Fill Or Kill
    DAY = 3   // Day order
};

/// Connection status
enum class ConnectionStatus : uint8_t {
    DISCONNECTED = 0,
    CONNECTING = 1,
    CONNECTED = 2,
    RECONNECTING = 3,
    ERROR = 4
};

/// Per-gateway statistics counters.
struct GatewayStatistics {
    uint64_t messages_received = 0;
    uint64_t ticks_processed = 0;
    uint64_t errors_count = 0;
    uint64_t reconnections = 0;
    std::chrono::microseconds avg_latency{0};
    std::chrono::steady_clock::time_point last_message_time;
};

/// Error severity levels
enum class ErrorSeverity : uint8_t {
    DEBUG = 0,
    INFO = 1,
    WARNING = 2,
    ERROR = 3,
    CRITICAL = 4
};

/// Log levels
enum class LogLevel : uint8_t {
    TRACE = 0,
    DEBUG = 1,
    INFO = 2,
    WARNING = 3,
    ERROR = 4,
    CRITICAL = 5
};

/// Event types for logging and replay
enum class EventType : uint8_t {
    TICK = 0,
    ORDER = 1,
    FILL = 2,
    SNAPSHOT = 3,
    TIMER = 4
};

/// Order status
enum class OrderStatus : uint8_t {
    PENDING = 0,
    ACCEPTED = 1,
    REJECTED = 2,
    PARTIALLY_FILLED = 3,
    FILLED = 4,
    CANCELLED = 5,
    EXPIRED = 6
};

// ============================================================================
// Market Data Structures
// ============================================================================

/// Market data tick (fixed size: 64 bytes for binary logging)
struct alignas(64) Tick {
    uint64_t exchange_timestamp_us;  ///< Exchange timestamp (microseconds)
    uint64_t receive_timestamp_us;   ///< Local receive timestamp (microseconds)
    uint32_t exchange_id;            ///< Exchange identifier
    uint32_t symbol_id;              ///< Symbol identifier
    Decimal price;                   ///< Tick price (8 bytes)
    Decimal quantity;                ///< Tick quantity (8 bytes)
    TickSide side;                   ///< BID / ASK / TRADE
    uint8_t flags;                   ///< Additional flags
    uint8_t reserved[22];            ///< Reserved for future use

    Tick() : exchange_timestamp_us(0), receive_timestamp_us(0),
             exchange_id(0), symbol_id(0), price(0), quantity(0),
             side(TickSide::BID), flags(0), reserved{} {}
};

static_assert(sizeof(Tick) == 64, "Tick must be 64 bytes");

/// Price level in order book
struct PriceLevel {
    Decimal price;
    Decimal quantity;
    
    PriceLevel() : price(0), quantity(0) {}
    PriceLevel(Decimal p, Decimal q) : price(p), quantity(q) {}
    
    // Convenience constructor from double
    PriceLevel(double p, double q) : price(toDecimal(p)), quantity(toDecimal(q)) {}
};

/// Order book snapshot
struct OrderBookSnapshot {
    uint32_t symbol_id;
    uint64_t timestamp_us;
    std::vector<PriceLevel> bids;  ///< Sorted best to worst
    std::vector<PriceLevel> asks;  ///< Sorted best to worst
    
    OrderBookSnapshot() : symbol_id(0), timestamp_us(0) {}
};

// ============================================================================
// Trading Structures
// ============================================================================

/// Order request (fixed size: 128 bytes for binary logging)
///
/// Field layout optimised for zero internal padding:
///   8B-aligned: order_id, timestamp_us, price, quantity, stop_price, client_order_id
///   4B-aligned: symbol_id, exchange_id, strategy_id
///   1B:         side, type, tif, strategy_flags
///   tail:       reserved[64]
struct alignas(64) OrderRequest {
    uint64_t order_id;          // offset  0 (8B)
    uint64_t timestamp_us;      // offset  8 (8B)
    Decimal price;              // offset 16 (8B)
    Decimal quantity;           // offset 24 (8B)
    Decimal stop_price;         // offset 32 (8B)  — trigger price for STOP orders
    uint64_t client_order_id;   // offset 40 (8B)  — for exchange reconciliation
    uint32_t symbol_id;         // offset 48 (4B)
    uint32_t exchange_id;       // offset 52 (4B)
    uint32_t strategy_id;       // offset 56 (4B)  — for fill routing / onFill
    OrderSide side;             // offset 60 (1B)
    OrderType type;             // offset 61 (1B)
    TimeInForce tif;            // offset 62 (1B)
    uint8_t strategy_flags;     // offset 63 (1B)  — bit0: reduce_only, bit1: post_only
    uint8_t reserved[64];       // offset 64 (64B) — reserved for future use

    OrderRequest() : order_id(0), timestamp_us(0),
                     price(0), quantity(0), stop_price(0), client_order_id(0),
                     symbol_id(0), exchange_id(0), strategy_id(0),
                     side(OrderSide::BUY), type(OrderType::LIMIT),
                     tif(TimeInForce::GTC), strategy_flags(0), reserved{} {}
};

static_assert(sizeof(OrderRequest) == 128, "OrderRequest must be 128 bytes (cache-aligned)");

/// Order result
struct OrderResult {
    bool success;
    std::string error_message;
    uint64_t exchange_order_id;
    uint64_t send_timestamp_us;
    
    OrderResult() : success(false), exchange_order_id(0), send_timestamp_us(0) {}
};

/// Fill event (fixed size: 128 bytes for binary logging)
///
/// Field layout optimised for zero internal padding:
///   8B-aligned: execution_id, fill_id, order_id, exchange_timestamp_us,
///               receive_timestamp_us, fill_price, fill_quantity
///   4B-aligned: symbol_id, exchange_id, strategy_id
///   1B:         side, is_maker
///   tail:       reserved[58]
struct alignas(64) Fill {
    uint64_t execution_id;          // offset  0 (8B)  — exchange trade_id
    uint64_t fill_id;               // offset  8 (8B)
    uint64_t order_id;              // offset 16 (8B)
    uint64_t exchange_timestamp_us;  // offset 24 (8B)
    uint64_t receive_timestamp_us;   // offset 32 (8B)
    Decimal fill_price;              // offset 40 (8B)
    Decimal fill_quantity;           // offset 48 (8B)
    uint32_t symbol_id;              // offset 56 (4B)
    uint32_t exchange_id;            // offset 60 (4B)
    uint32_t strategy_id;            // offset 64 (4B)  — for onFill routing
    OrderSide side;                  // offset 68 (1B)
    uint8_t is_maker;                // offset 69 (1B)  — 1 if maker, 0 if taker
    uint8_t reserved[58];            // offset 70 (58B) — reserved for future use

    Fill() : execution_id(0), fill_id(0), order_id(0),
             exchange_timestamp_us(0), receive_timestamp_us(0),
             fill_price(0), fill_quantity(0),
             symbol_id(0), exchange_id(0), strategy_id(0),
             side(OrderSide::BUY), is_maker(0), reserved{} {}
};

static_assert(sizeof(Fill) == 128, "Fill must be 128 bytes (cache-aligned)");

/// Order acknowledgment
struct OrderAck {
    uint64_t order_id;
    uint64_t exchange_order_id;
    OrderStatus status;
    uint64_t timestamp_us;
    
    OrderAck() : order_id(0), exchange_order_id(0),
                status(OrderStatus::PENDING), timestamp_us(0) {}
};

/// Order rejection
struct OrderReject {
    uint64_t order_id;
    std::string reason;
    uint64_t timestamp_us;
    
    OrderReject() : order_id(0), timestamp_us(0) {}
};

// ============================================================================
// Position Management
// ============================================================================

/// Position with P&L calculation
struct Position {
    uint32_t symbol_id;
    Decimal quantity;           ///< Positive for long, negative for short
    Decimal average_price;      ///< Average entry price
    Decimal realized_pnl;       ///< Realized profit/loss
    uint64_t last_update_us;
    
    Position() : symbol_id(0), quantity(0), average_price(0),
                realized_pnl(0), last_update_us(0) {}
    
    /// Calculate unrealized P&L given current market price
    Decimal getUnrealizedPnL(Decimal current_price) const {
        return quantity * (current_price - average_price);
    }
    
    /// Get total P&L (realized + unrealized)
    Decimal getTotalPnL(Decimal current_price) const {
        return realized_pnl + getUnrealizedPnL(current_price);
    }
    
    /// Check if position is flat (no position)
    bool isFlat() const {
        return quantity == Decimal(0);
    }
    
    /// Check if position is long
    bool isLong() const {
        return quantity > Decimal(0);
    }
    
    /// Check if position is short
    bool isShort() const {
        return quantity < Decimal(0);
    }
};

// ============================================================================
// Helper Functions
// ============================================================================

/// Convert OrderSide to string
inline const char* toString(OrderSide side) {
    switch (side) {
        case OrderSide::BUY: return "BUY";
        case OrderSide::SELL: return "SELL";
        default: return "UNKNOWN";
    }
}

/// Convert OrderType to string
inline const char* toString(OrderType type) {
    switch (type) {
        case OrderType::LIMIT: return "LIMIT";
        case OrderType::MARKET: return "MARKET";
        case OrderType::STOP_LOSS: return "STOP_LOSS";
        case OrderType::STOP_LIMIT: return "STOP_LIMIT";
        default: return "UNKNOWN";
    }
}

/// Convert TimeInForce to string
inline const char* toString(TimeInForce tif) {
    switch (tif) {
        case TimeInForce::GTC: return "GTC";
        case TimeInForce::IOC: return "IOC";
        case TimeInForce::FOK: return "FOK";
        case TimeInForce::DAY: return "DAY";
        default: return "UNKNOWN";
    }
}

/// Convert OrderStatus to string
inline const char* toString(OrderStatus status) {
    switch (status) {
        case OrderStatus::PENDING: return "PENDING";
        case OrderStatus::ACCEPTED: return "ACCEPTED";
        case OrderStatus::REJECTED: return "REJECTED";
        case OrderStatus::PARTIALLY_FILLED: return "PARTIALLY_FILLED";
        case OrderStatus::FILLED: return "FILLED";
        case OrderStatus::CANCELLED: return "CANCELLED";
        case OrderStatus::EXPIRED: return "EXPIRED";
        default: return "UNKNOWN";
    }
}

/// Convert ConnectionStatus to string
inline const char* toString(ConnectionStatus status) {
    switch (status) {
        case ConnectionStatus::DISCONNECTED: return "DISCONNECTED";
        case ConnectionStatus::CONNECTING: return "CONNECTING";
        case ConnectionStatus::CONNECTED: return "CONNECTED";
        case ConnectionStatus::RECONNECTING: return "RECONNECTING";
        case ConnectionStatus::ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

/// Convert ErrorSeverity to string
inline const char* toString(ErrorSeverity severity) {
    switch (severity) {
        case ErrorSeverity::DEBUG: return "DEBUG";
        case ErrorSeverity::INFO: return "INFO";
        case ErrorSeverity::WARNING: return "WARNING";
        case ErrorSeverity::ERROR: return "ERROR";
        case ErrorSeverity::CRITICAL: return "CRITICAL";
        default: return "UNKNOWN";
    }
}

} // namespace chronos
