#pragma once

#include "chronos/core/types.hpp"
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <chrono>
#include <memory>

namespace chronos {

// ============================================================================
// Error Categories
// ============================================================================

enum class ErrorCategory {
    NETWORK,           ///< Network/connection errors
    MARKET_DATA,       ///< Market data parsing/validation errors
    ORDER_EXECUTION,   ///< Order execution errors
    RISK_MANAGEMENT,   ///< Risk check failures
    CONFIGURATION,     ///< Configuration errors
    PERSISTENCE,       ///< File I/O and persistence errors
    SYSTEM,            ///< System-level errors (memory, threads, etc.)
    STRATEGY,          ///< Strategy execution errors
    UNKNOWN            ///< Unknown error category
};

/// Convert ErrorCategory to string
const char* toString(ErrorCategory category);

// ============================================================================
// Recovery Actions
// ============================================================================

enum class RecoveryAction {
    NONE,              ///< No recovery action needed
    RETRY,             ///< Retry the operation
    RECONNECT,         ///< Reconnect to exchange
    RELOAD_CONFIG,     ///< Reload configuration
    RESTART_COMPONENT, ///< Restart specific component
    SHUTDOWN,          ///< Graceful shutdown required
    ALERT_OPERATOR     ///< Alert human operator
};

/// Convert RecoveryAction to string
const char* toString(RecoveryAction action);

// ============================================================================
// Error Structure
// ============================================================================

/// Comprehensive error information
struct Error {
    ErrorSeverity severity;           ///< Error severity level
    ErrorCategory category;           ///< Error category
    std::string component;            ///< Component that generated the error
    std::string message;              ///< Human-readable error message
    std::string context;              ///< Additional context information
    uint64_t timestamp_us;            ///< Error timestamp (microseconds)
    std::vector<std::string> stack_trace; ///< Stack trace (if available)
    RecoveryAction recovery_action;   ///< Suggested recovery action
    int error_code;                   ///< Numeric error code (optional)
    
    /// Constructor
    Error(ErrorSeverity sev = ErrorSeverity::ERROR,
          ErrorCategory cat = ErrorCategory::UNKNOWN,
          const std::string& comp = "",
          const std::string& msg = "",
          const std::string& ctx = "",
          RecoveryAction recovery = RecoveryAction::NONE,
          int code = 0);
    
    /// Get formatted error string
    std::string toString() const;
    
    /// Check if error is critical (requires immediate action)
    bool isCritical() const;
    
    /// Get current timestamp in microseconds
    static uint64_t getCurrentTimestamp();
};

// ============================================================================
// Error Statistics
// ============================================================================

/// Statistics for error tracking
struct ErrorStatistics {
    uint64_t total_errors;
    uint64_t errors_by_severity[5];  ///< Count per severity level
    uint64_t errors_by_category[9];  ///< Count per category
    uint64_t last_error_timestamp_us;
    
    ErrorStatistics();
    void reset();
    void recordError(const Error& error);
    std::string toString() const;
};

// ============================================================================
// Error Handler
// ============================================================================

/// Callback function type for error notifications
using ErrorCallback = std::function<void(const Error&)>;

/// Central error handling and logging system
class ErrorHandler {
public:
    /// Get singleton instance
    static ErrorHandler& getInstance();
    
    /// Delete copy constructor and assignment operator
    ErrorHandler(const ErrorHandler&) = delete;
    ErrorHandler& operator=(const ErrorHandler&) = delete;
    
    /// Report an error
    void reportError(const Error& error);
    
    /// Report an error with simplified parameters
    void reportError(ErrorSeverity severity,
                    ErrorCategory category,
                    const std::string& component,
                    const std::string& message,
                    const std::string& context = "",
                    RecoveryAction recovery = RecoveryAction::NONE,
                    int error_code = 0);
    
    /// Register error callback
    /// Returns callback ID for later removal
    uint64_t registerCallback(ErrorCallback callback);
    
    /// Unregister error callback
    void unregisterCallback(uint64_t callback_id);
    
    /// Register category-specific callback
    uint64_t registerCategoryCallback(ErrorCategory category, ErrorCallback callback);
    
    /// Unregister category-specific callback
    void unregisterCategoryCallback(ErrorCategory category, uint64_t callback_id);
    
    /// Get error statistics
    ErrorStatistics getStatistics() const;
    
    /// Reset error statistics
    void resetStatistics();
    
    /// Get recent errors (up to max_count)
    std::vector<Error> getRecentErrors(size_t max_count = 100) const;
    
    /// Clear error history
    void clearHistory();
    
    /// Set maximum error history size
    void setMaxHistorySize(size_t size);
    
    /// Enable/disable error logging to console
    void setConsoleLogging(bool enabled);
    
    /// Set minimum severity level for callbacks
    void setMinimumSeverity(ErrorSeverity severity);
    
private:
    ErrorHandler();
    ~ErrorHandler() = default;
    
    /// Invoke all registered callbacks
    void invokeCallbacks(const Error& error);
    
    /// Add error to history
    void addToHistory(const Error& error);
    
    mutable std::mutex mutex_;
    
    /// Global callbacks (called for all errors)
    std::unordered_map<uint64_t, ErrorCallback> callbacks_;
    
    /// Category-specific callbacks
    std::unordered_map<ErrorCategory, std::unordered_map<uint64_t, ErrorCallback>> category_callbacks_;
    
    /// Error statistics
    ErrorStatistics statistics_;
    
    /// Error history (circular buffer)
    std::vector<Error> error_history_;
    size_t history_write_index_;
    size_t max_history_size_;
    
    /// Next callback ID
    uint64_t next_callback_id_;
    
    /// Configuration
    bool console_logging_enabled_;
    ErrorSeverity minimum_severity_;
};

// ============================================================================
// Convenience Macros
// ============================================================================

#define CHRONOS_ERROR(category, component, message) \
    chronos::ErrorHandler::getInstance().reportError( \
        chronos::ErrorSeverity::ERROR, \
        category, \
        component, \
        message)

#define CHRONOS_WARNING(category, component, message) \
    chronos::ErrorHandler::getInstance().reportError( \
        chronos::ErrorSeverity::WARNING, \
        category, \
        component, \
        message)

#define CHRONOS_CRITICAL(category, component, message) \
    chronos::ErrorHandler::getInstance().reportError( \
        chronos::ErrorSeverity::CRITICAL, \
        category, \
        component, \
        message)

#define CHRONOS_FATAL(category, component, message) \
    chronos::ErrorHandler::getInstance().reportError( \
        chronos::ErrorSeverity::FATAL, \
        category, \
        component, \
        message)

} // namespace chronos
