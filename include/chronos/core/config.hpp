#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace chronos {

using json = nlohmann::json;

// ============================================================================
// Configuration Structures
// ============================================================================

/// Exchange configuration
struct ExchangeConfig {
    std::string name;
    std::string websocket_url;
    std::string rest_url;          // REST API base URL (e.g. https://testnet.binancefuture.com)
    std::string user_stream_url;   // User data stream WS URL (e.g. wss://stream.binancefuture.com/ws)
    std::string proxy_host;        // HTTP CONNECT proxy host (e.g. 127.0.0.1)
    uint16_t proxy_port = 0;       // proxy port (0 = no proxy, direct connect)
    std::string api_key;
    std::string api_secret;
    std::vector<std::string> symbols;
    uint32_t reconnect_interval_ms;
    uint32_t heartbeat_interval_ms;
    uint32_t timeout_ms;
    
    ExchangeConfig() : reconnect_interval_ms(5000),
                      heartbeat_interval_ms(30000),
                      timeout_ms(10000) {}
    
    /// Validate configuration
    bool validate() const;
    
    /// Convert to JSON
    json toJson() const;
    
    /// Create from JSON
    static ExchangeConfig fromJson(const json& j);
};

/// Strategy configuration
struct StrategyConfig {
    std::string name;
    std::string library_path;
    json parameters;
    std::vector<uint32_t> symbol_ids;
    bool enabled;
    
    StrategyConfig() : enabled(true) {}
    
    /// Validate configuration
    bool validate() const;
    
    /// Convert to JSON
    json toJson() const;
    
    /// Create from JSON
    static StrategyConfig fromJson(const json& j);
};

/// Risk parameters
struct RiskParameters {
    double max_order_value;           ///< Maximum single order value
    double max_position_value;        ///< Maximum position value per symbol
    uint32_t max_orders_per_second;   ///< Rate limit
    double max_total_position_value;  ///< Maximum total portfolio value
    double min_available_capital;     ///< Minimum capital to maintain
    double max_drawdown_percent;      ///< Maximum drawdown percentage
    
    RiskParameters() : max_order_value(100000.0),
                      max_position_value(500000.0),
                      max_orders_per_second(100),
                      max_total_position_value(1000000.0),
                      min_available_capital(10000.0),
                      max_drawdown_percent(20.0) {}
    
    /// Validate parameters
    bool validate() const;
    
    /// Convert to JSON
    json toJson() const;
    
    /// Create from JSON
    static RiskParameters fromJson(const json& j);
};

/// Logging configuration
struct LogConfig {
    std::string log_dir;
    size_t buffer_size;           ///< Buffer size for each log type
    uint32_t flush_interval_ms;   ///< Flush interval in milliseconds
    uint32_t retention_days;      ///< Log retention period
    bool compress_old_logs;       ///< Compress logs older than 7 days
    bool enable_tick_logging;
    bool enable_order_logging;
    bool enable_fill_logging;
    bool enable_snapshot_logging;
    
    LogConfig() : log_dir("./logs"),
                 buffer_size(65536),
                 flush_interval_ms(100),
                 retention_days(30),
                 compress_old_logs(true),
                 enable_tick_logging(true),
                 enable_order_logging(true),
                 enable_fill_logging(true),
                 enable_snapshot_logging(false) {}
    
    /// Validate configuration
    bool validate() const;
    
    /// Convert to JSON
    json toJson() const;
    
    /// Create from JSON
    static LogConfig fromJson(const json& j);
};

/// ZMQ configuration
struct ZMQConfig {
    std::string publish_endpoint;
    uint32_t high_water_mark;
    uint32_t linger_ms;
    
    ZMQConfig() : publish_endpoint("tcp://*:5555"),
                 high_water_mark(10000),
                 linger_ms(1000) {}
    
    /// Validate configuration
    bool validate() const;
    
    /// Convert to JSON
    json toJson() const;
    
    /// Create from JSON
    static ZMQConfig fromJson(const json& j);
};

/// Performance monitoring configuration
struct MonitoringConfig {
    bool enable_metrics;
    uint16_t metrics_port;
    bool enable_health_check;
    uint16_t health_check_port;
    uint32_t metrics_update_interval_ms;
    
    MonitoringConfig() : enable_metrics(true),
                        metrics_port(9090),
                        enable_health_check(true),
                        health_check_port(8080),
                        metrics_update_interval_ms(1000) {}
    
    /// Validate configuration
    bool validate() const;
    
    /// Convert to JSON
    json toJson() const;
    
    /// Create from JSON
    static MonitoringConfig fromJson(const json& j);
};

/// System configuration (aggregates all configurations)
struct SystemConfig {
    std::vector<ExchangeConfig> exchanges;
    std::vector<StrategyConfig> strategies;
    RiskParameters risk_parameters;
    LogConfig log_config;
    ZMQConfig zmq_config;
    MonitoringConfig monitoring_config;
    
    double initial_capital;
    std::string state_file;  ///< File for persisting system state
    
    SystemConfig() : initial_capital(1000000.0),
                    state_file("./state/system_state.json") {}
    
    /// Validate entire configuration
    bool validate() const;
    
    /// Convert to JSON
    json toJson() const;
    
    /// Create from JSON
    static SystemConfig fromJson(const json& j);
    
    /// Load from file
    static SystemConfig loadFromFile(const std::string& filepath);
    
    /// Save to file
    bool saveToFile(const std::string& filepath) const;
};

// ============================================================================
// JSON Serialization Support
// ============================================================================

/// JSON serialization for ExchangeConfig
void to_json(json& j, const ExchangeConfig& config);
void from_json(const json& j, ExchangeConfig& config);

/// JSON serialization for StrategyConfig
void to_json(json& j, const StrategyConfig& config);
void from_json(const json& j, StrategyConfig& config);

/// JSON serialization for RiskParameters
void to_json(json& j, const RiskParameters& params);
void from_json(const json& j, RiskParameters& params);

/// JSON serialization for LogConfig
void to_json(json& j, const LogConfig& config);
void from_json(const json& j, LogConfig& config);

/// JSON serialization for ZMQConfig
void to_json(json& j, const ZMQConfig& config);
void from_json(const json& j, ZMQConfig& config);

/// JSON serialization for MonitoringConfig
void to_json(json& j, const MonitoringConfig& config);
void from_json(const json& j, MonitoringConfig& config);

/// JSON serialization for SystemConfig
void to_json(json& j, const SystemConfig& config);
void from_json(const json& j, SystemConfig& config);

} // namespace chronos
