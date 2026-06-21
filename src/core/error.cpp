#include "chronos/core/error.hpp"
#include <sstream>
#include <iomanip>
#include <iostream>
#include <ctime>

namespace chronos {

// ============================================================================
// ErrorCategory String Conversion
// ============================================================================

const char* toString(ErrorCategory category) {
    switch (category) {
        case ErrorCategory::NETWORK:          return "NETWORK";
        case ErrorCategory::MARKET_DATA:      return "MARKET_DATA";
        case ErrorCategory::ORDER_EXECUTION:  return "ORDER_EXECUTION";
        case ErrorCategory::RISK_MANAGEMENT:  return "RISK_MANAGEMENT";
        case ErrorCategory::CONFIGURATION:    return "CONFIGURATION";
        case ErrorCategory::PERSISTENCE:      return "PERSISTENCE";
        case ErrorCategory::SYSTEM:           return "SYSTEM";
        case ErrorCategory::STRATEGY:         return "STRATEGY";
        case ErrorCategory::UNKNOWN:          return "UNKNOWN";
        default:                              return "INVALID";
    }
}

// ============================================================================
// RecoveryAction String Conversion
// ============================================================================

const char* toString(RecoveryAction action) {
    switch (action) {
        case RecoveryAction::NONE:              return "NONE";
        case RecoveryAction::RETRY:             return "RETRY";
        case RecoveryAction::RECONNECT:         return "RECONNECT";
        case RecoveryAction::RELOAD_CONFIG:     return "RELOAD_CONFIG";
        case RecoveryAction::RESTART_COMPONENT: return "RESTART_COMPONENT";
        case RecoveryAction::SHUTDOWN:          return "SHUTDOWN";
        case RecoveryAction::ALERT_OPERATOR:    return "ALERT_OPERATOR";
        default:                                return "INVALID";
    }
}

// ============================================================================
// Error Implementation
// ============================================================================

Error::Error(ErrorSeverity sev,
             ErrorCategory cat,
             const std::string& comp,
             const std::string& msg,
             const std::string& ctx,
             RecoveryAction recovery,
             int code)
    : severity(sev),
      category(cat),
      component(comp),
      message(msg),
      context(ctx),
      timestamp_us(getCurrentTimestamp()),
      recovery_action(recovery),
      error_code(code) {
}

std::string Error::toString() const {
    std::ostringstream oss;
    
    // Format timestamp
    uint64_t timestamp_sec = timestamp_us / 1000000;
    uint64_t timestamp_usec = timestamp_us % 1000000;
    std::time_t time = static_cast<std::time_t>(timestamp_sec);
    std::tm* tm_info = std::localtime(&time);
    
    char time_buffer[64];
    std::strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", tm_info);
    
    oss << "[" << time_buffer << "." << std::setfill('0') << std::setw(6) << timestamp_usec << "] ";
    oss << "[" << chronos::toString(severity) << "] ";
    oss << "[" << chronos::toString(category) << "] ";
    
    if (!component.empty()) {
        oss << "[" << component << "] ";
    }
    
    oss << message;
    
    if (!context.empty()) {
        oss << " | Context: " << context;
    }
    
    if (error_code != 0) {
        oss << " | Code: " << error_code;
    }
    
    if (recovery_action != RecoveryAction::NONE) {
        oss << " | Recovery: " << chronos::toString(recovery_action);
    }
    
    if (!stack_trace.empty()) {
        oss << "\nStack trace:\n";
        for (const auto& frame : stack_trace) {
            oss << "  " << frame << "\n";
        }
    }
    
    return oss.str();
}

bool Error::isCritical() const {
    return severity == ErrorSeverity::CRITICAL;
}

uint64_t Error::getCurrentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
}

// ============================================================================
// ErrorStatistics Implementation
// ============================================================================

ErrorStatistics::ErrorStatistics()
    : total_errors(0),
      last_error_timestamp_us(0) {
    for (int i = 0; i < 5; ++i) {
        errors_by_severity[i] = 0;
    }
    for (int i = 0; i < 9; ++i) {
        errors_by_category[i] = 0;
    }
}

void ErrorStatistics::reset() {
    total_errors = 0;
    last_error_timestamp_us = 0;
    for (int i = 0; i < 5; ++i) {
        errors_by_severity[i] = 0;
    }
    for (int i = 0; i < 9; ++i) {
        errors_by_category[i] = 0;
    }
}

void ErrorStatistics::recordError(const Error& error) {
    ++total_errors;
    last_error_timestamp_us = error.timestamp_us;
    
    // Record by severity
    int sev_index = static_cast<int>(error.severity);
    if (sev_index >= 0 && sev_index < 5) {
        ++errors_by_severity[sev_index];
    }
    
    // Record by category
    int cat_index = static_cast<int>(error.category);
    if (cat_index >= 0 && cat_index < 9) {
        ++errors_by_category[cat_index];
    }
}

std::string ErrorStatistics::toString() const {
    std::ostringstream oss;
    oss << "Error Statistics:\n";
    oss << "  Total Errors: " << total_errors << "\n";
    oss << "  By Severity:\n";
    oss << "    DEBUG:    " << errors_by_severity[0] << "\n";
    oss << "    INFO:     " << errors_by_severity[1] << "\n";
    oss << "    WARNING:  " << errors_by_severity[2] << "\n";
    oss << "    ERROR:    " << errors_by_severity[3] << "\n";
    oss << "    CRITICAL: " << errors_by_severity[4] << "\n";
    oss << "  By Category:\n";
    oss << "    NETWORK:          " << errors_by_category[0] << "\n";
    oss << "    MARKET_DATA:      " << errors_by_category[1] << "\n";
    oss << "    ORDER_EXECUTION:  " << errors_by_category[2] << "\n";
    oss << "    RISK_MANAGEMENT:  " << errors_by_category[3] << "\n";
    oss << "    CONFIGURATION:    " << errors_by_category[4] << "\n";
    oss << "    PERSISTENCE:      " << errors_by_category[5] << "\n";
    oss << "    SYSTEM:           " << errors_by_category[6] << "\n";
    oss << "    STRATEGY:         " << errors_by_category[7] << "\n";
    oss << "    UNKNOWN:          " << errors_by_category[8] << "\n";
    return oss.str();
}

// ============================================================================
// ErrorHandler Implementation
// ============================================================================

ErrorHandler::ErrorHandler()
    : history_write_index_(0),
      max_history_size_(1000),
      next_callback_id_(1),
      console_logging_enabled_(true),
      minimum_severity_(ErrorSeverity::DEBUG) {
    error_history_.reserve(max_history_size_);
}

ErrorHandler& ErrorHandler::getInstance() {
    static ErrorHandler instance;
    return instance;
}

void ErrorHandler::reportError(const Error& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Check minimum severity
    if (error.severity < minimum_severity_) {
        return;
    }
    
    // Update statistics
    statistics_.recordError(error);
    
    // Add to history
    addToHistory(error);
    
    // Console logging
    if (console_logging_enabled_) {
        std::cerr << error.toString() << std::endl;
    }
    
    // Invoke callbacks
    invokeCallbacks(error);
}

void ErrorHandler::reportError(ErrorSeverity severity,
                               ErrorCategory category,
                               const std::string& component,
                               const std::string& message,
                               const std::string& context,
                               RecoveryAction recovery,
                               int error_code) {
    Error error(severity, category, component, message, context, recovery, error_code);
    reportError(error);
}

uint64_t ErrorHandler::registerCallback(ErrorCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t id = next_callback_id_++;
    callbacks_[id] = std::move(callback);
    return id;
}

void ErrorHandler::unregisterCallback(uint64_t callback_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    callbacks_.erase(callback_id);
}

uint64_t ErrorHandler::registerCategoryCallback(ErrorCategory category, ErrorCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t id = next_callback_id_++;
    category_callbacks_[category][id] = std::move(callback);
    return id;
}

void ErrorHandler::unregisterCategoryCallback(ErrorCategory category, uint64_t callback_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = category_callbacks_.find(category);
    if (it != category_callbacks_.end()) {
        it->second.erase(callback_id);
    }
}

ErrorStatistics ErrorHandler::getStatistics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return statistics_;
}

void ErrorHandler::resetStatistics() {
    std::lock_guard<std::mutex> lock(mutex_);
    statistics_.reset();
}

std::vector<Error> ErrorHandler::getRecentErrors(size_t max_count) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<Error> result;
    size_t count = std::min(max_count, error_history_.size());
    result.reserve(count);
    
    // Read from circular buffer in reverse order (most recent first)
    for (size_t i = 0; i < count; ++i) {
        size_t index = (history_write_index_ + error_history_.size() - 1 - i) % error_history_.size();
        if (index < error_history_.size()) {
            result.push_back(error_history_[index]);
        }
    }
    
    return result;
}

void ErrorHandler::clearHistory() {
    std::lock_guard<std::mutex> lock(mutex_);
    error_history_.clear();
    history_write_index_ = 0;
}

void ErrorHandler::setMaxHistorySize(size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_history_size_ = size;
    if (error_history_.size() > max_history_size_) {
        error_history_.resize(max_history_size_);
        history_write_index_ = 0;
    }
}

void ErrorHandler::setConsoleLogging(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    console_logging_enabled_ = enabled;
}

void ErrorHandler::setMinimumSeverity(ErrorSeverity severity) {
    std::lock_guard<std::mutex> lock(mutex_);
    minimum_severity_ = severity;
}

void ErrorHandler::invokeCallbacks(const Error& error) {
    // Invoke global callbacks
    for (const auto& [id, callback] : callbacks_) {
        try {
            callback(error);
        } catch (const std::exception& e) {
            std::cerr << "Exception in error callback: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "Unknown exception in error callback" << std::endl;
        }
    }
    
    // Invoke category-specific callbacks
    auto it = category_callbacks_.find(error.category);
    if (it != category_callbacks_.end()) {
        for (const auto& [id, callback] : it->second) {
            try {
                callback(error);
            } catch (const std::exception& e) {
                std::cerr << "Exception in category error callback: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "Unknown exception in category error callback" << std::endl;
            }
        }
    }
}

void ErrorHandler::addToHistory(const Error& error) {
    if (error_history_.size() < max_history_size_) {
        error_history_.push_back(error);
        history_write_index_ = error_history_.size();
    } else {
        // Circular buffer: overwrite oldest entry
        error_history_[history_write_index_] = error;
        history_write_index_ = (history_write_index_ + 1) % max_history_size_;
    }
}

} // namespace chronos
