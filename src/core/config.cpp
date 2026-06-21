#include "chronos/core/config.hpp"
#include <fstream>
#include <stdexcept>

namespace chronos {

// ============================================================================
// ExchangeConfig Implementation
// ============================================================================

bool ExchangeConfig::validate() const {
    if (name.empty()) {
        return false;
    }
    if (websocket_url.empty()) {
        return false;
    }
    if (symbols.empty()) {
        return false;
    }
    if (reconnect_interval_ms == 0 || reconnect_interval_ms > 60000) {
        return false;
    }
    if (heartbeat_interval_ms == 0 || heartbeat_interval_ms > 120000) {
        return false;
    }
    if (timeout_ms == 0 || timeout_ms > 60000) {
        return false;
    }
    return true;
}

json ExchangeConfig::toJson() const {
    json j;
    to_json(j, *this);
    return j;
}

ExchangeConfig ExchangeConfig::fromJson(const json& j) {
    ExchangeConfig config;
    from_json(j, config);
    return config;
}

// ============================================================================
// StrategyConfig Implementation
// ============================================================================

bool StrategyConfig::validate() const {
    if (name.empty()) {
        return false;
    }
    // library_path is optional — only required for dynamically loaded strategies
    if (symbol_ids.empty()) {
        return false;
    }
    return true;
}

json StrategyConfig::toJson() const {
    json j;
    to_json(j, *this);
    return j;
}

StrategyConfig StrategyConfig::fromJson(const json& j) {
    StrategyConfig config;
    from_json(j, config);
    return config;
}

// ============================================================================
// RiskParameters Implementation
// ============================================================================

bool RiskParameters::validate() const {
    if (max_order_value <= 0.0) {
        return false;
    }
    if (max_position_value <= 0.0) {
        return false;
    }
    if (max_orders_per_second == 0 || max_orders_per_second > 10000) {
        return false;
    }
    if (max_total_position_value <= 0.0) {
        return false;
    }
    if (min_available_capital < 0.0) {
        return false;
    }
    if (max_drawdown_percent <= 0.0 || max_drawdown_percent > 100.0) {
        return false;
    }
    // Consistency checks
    if (max_order_value > max_position_value) {
        return false;
    }
    if (max_position_value > max_total_position_value) {
        return false;
    }
    return true;
}

json RiskParameters::toJson() const {
    json j;
    to_json(j, *this);
    return j;
}

RiskParameters RiskParameters::fromJson(const json& j) {
    RiskParameters params;
    from_json(j, params);
    return params;
}

// ============================================================================
// LogConfig Implementation
// ============================================================================

bool LogConfig::validate() const {
    if (log_dir.empty()) {
        return false;
    }
    if (buffer_size == 0 || buffer_size > 10485760) { // Max 10MB
        return false;
    }
    if (flush_interval_ms == 0 || flush_interval_ms > 10000) {
        return false;
    }
    if (retention_days == 0 || retention_days > 3650) { // Max 10 years
        return false;
    }
    return true;
}

json LogConfig::toJson() const {
    json j;
    to_json(j, *this);
    return j;
}

LogConfig LogConfig::fromJson(const json& j) {
    LogConfig config;
    from_json(j, config);
    return config;
}

// ============================================================================
// ZMQConfig Implementation
// ============================================================================

bool ZMQConfig::validate() const {
    if (publish_endpoint.empty()) {
        return false;
    }
    if (high_water_mark == 0 || high_water_mark > 1000000) {
        return false;
    }
    if (linger_ms > 30000) { // Max 30 seconds
        return false;
    }
    return true;
}

json ZMQConfig::toJson() const {
    json j;
    to_json(j, *this);
    return j;
}

ZMQConfig ZMQConfig::fromJson(const json& j) {
    ZMQConfig config;
    from_json(j, config);
    return config;
}

// ============================================================================
// MonitoringConfig Implementation
// ============================================================================

bool MonitoringConfig::validate() const {
    if (enable_metrics && (metrics_port == 0 || metrics_port > 65535)) {
        return false;
    }
    if (enable_health_check && (health_check_port == 0 || health_check_port > 65535)) {
        return false;
    }
    if (enable_metrics && enable_health_check && metrics_port == health_check_port) {
        return false; // Ports must be different
    }
    if (metrics_update_interval_ms == 0 || metrics_update_interval_ms > 60000) {
        return false;
    }
    return true;
}

json MonitoringConfig::toJson() const {
    json j;
    to_json(j, *this);
    return j;
}

MonitoringConfig MonitoringConfig::fromJson(const json& j) {
    MonitoringConfig config;
    from_json(j, config);
    return config;
}

// ============================================================================
// SystemConfig Implementation
// ============================================================================

bool SystemConfig::validate() const {
    // Validate initial capital
    if (initial_capital <= 0.0) {
        return false;
    }
    
    // Validate state file path
    if (state_file.empty()) {
        return false;
    }
    
    // Validate all exchanges
    if (exchanges.empty()) {
        return false;
    }
    for (const auto& exchange : exchanges) {
        if (!exchange.validate()) {
            return false;
        }
    }
    
    // Validate all strategies
    if (strategies.empty()) {
        return false;
    }
    for (const auto& strategy : strategies) {
        if (!strategy.validate()) {
            return false;
        }
    }
    
    // Validate risk parameters
    if (!risk_parameters.validate()) {
        return false;
    }
    
    // Validate log config
    if (!log_config.validate()) {
        return false;
    }
    
    // Validate ZMQ config
    if (!zmq_config.validate()) {
        return false;
    }
    
    // Validate monitoring config
    if (!monitoring_config.validate()) {
        return false;
    }
    
    return true;
}

json SystemConfig::toJson() const {
    json j;
    to_json(j, *this);
    return j;
}

SystemConfig SystemConfig::fromJson(const json& j) {
    SystemConfig config;
    from_json(j, config);
    return config;
}

SystemConfig SystemConfig::loadFromFile(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open config file: " + filepath);
    }
    
    json j;
    try {
        file >> j;
    } catch (const json::exception& e) {
        throw std::runtime_error("Failed to parse JSON config: " + std::string(e.what()));
    }
    
    SystemConfig config = SystemConfig::fromJson(j);
    
    if (!config.validate()) {
        throw std::runtime_error("Invalid configuration in file: " + filepath);
    }
    
    return config;
}

bool SystemConfig::saveToFile(const std::string& filepath) const {
    if (!validate()) {
        return false;
    }
    
    std::ofstream file(filepath);
    if (!file.is_open()) {
        return false;
    }
    
    json j = toJson();
    file << j.dump(4); // Pretty print with 4-space indent
    
    return file.good();
}

// ============================================================================
// JSON Serialization Functions
// ============================================================================

void to_json(json& j, const ExchangeConfig& config) {
    j = json{
        {"name", config.name},
        {"websocket_url", config.websocket_url},
        {"rest_url", config.rest_url},
        {"user_stream_url", config.user_stream_url},
        {"proxy_host", config.proxy_host},
        {"proxy_port", config.proxy_port},
        {"api_key", config.api_key},
        {"api_secret", config.api_secret},
        {"symbols", config.symbols},
        {"reconnect_interval_ms", config.reconnect_interval_ms},
        {"heartbeat_interval_ms", config.heartbeat_interval_ms},
        {"timeout_ms", config.timeout_ms}
    };
}

void from_json(const json& j, ExchangeConfig& config) {
    j.at("name").get_to(config.name);
    j.at("websocket_url").get_to(config.websocket_url);
    j.at("api_key").get_to(config.api_key);
    j.at("api_secret").get_to(config.api_secret);
    j.at("symbols").get_to(config.symbols);

    if (j.contains("rest_url"))
        j.at("rest_url").get_to(config.rest_url);
    if (j.contains("user_stream_url"))
        j.at("user_stream_url").get_to(config.user_stream_url);
    if (j.contains("proxy_host"))
        j.at("proxy_host").get_to(config.proxy_host);
    if (j.contains("proxy_port"))
        j.at("proxy_port").get_to(config.proxy_port);
    
    if (j.contains("reconnect_interval_ms")) {
        j.at("reconnect_interval_ms").get_to(config.reconnect_interval_ms);
    }
    if (j.contains("heartbeat_interval_ms")) {
        j.at("heartbeat_interval_ms").get_to(config.heartbeat_interval_ms);
    }
    if (j.contains("timeout_ms")) {
        j.at("timeout_ms").get_to(config.timeout_ms);
    }
}

void to_json(json& j, const StrategyConfig& config) {
    j = json{
        {"name", config.name},
        {"library_path", config.library_path},
        {"parameters", config.parameters},
        {"symbol_ids", config.symbol_ids},
        {"enabled", config.enabled}
    };
}

void from_json(const json& j, StrategyConfig& config) {
    j.at("name").get_to(config.name);
    j.at("library_path").get_to(config.library_path);
    j.at("parameters").get_to(config.parameters);
    j.at("symbol_ids").get_to(config.symbol_ids);
    
    if (j.contains("enabled")) {
        j.at("enabled").get_to(config.enabled);
    }
}

void to_json(json& j, const RiskParameters& params) {
    j = json{
        {"max_order_value", params.max_order_value},
        {"max_position_value", params.max_position_value},
        {"max_orders_per_second", params.max_orders_per_second},
        {"max_total_position_value", params.max_total_position_value},
        {"min_available_capital", params.min_available_capital},
        {"max_drawdown_percent", params.max_drawdown_percent}
    };
}

void from_json(const json& j, RiskParameters& params) {
    j.at("max_order_value").get_to(params.max_order_value);
    j.at("max_position_value").get_to(params.max_position_value);
    j.at("max_orders_per_second").get_to(params.max_orders_per_second);
    j.at("max_total_position_value").get_to(params.max_total_position_value);
    j.at("min_available_capital").get_to(params.min_available_capital);
    j.at("max_drawdown_percent").get_to(params.max_drawdown_percent);
}

void to_json(json& j, const LogConfig& config) {
    j = json{
        {"log_dir", config.log_dir},
        {"buffer_size", config.buffer_size},
        {"flush_interval_ms", config.flush_interval_ms},
        {"retention_days", config.retention_days},
        {"compress_old_logs", config.compress_old_logs},
        {"enable_tick_logging", config.enable_tick_logging},
        {"enable_order_logging", config.enable_order_logging},
        {"enable_fill_logging", config.enable_fill_logging},
        {"enable_snapshot_logging", config.enable_snapshot_logging}
    };
}

void from_json(const json& j, LogConfig& config) {
    j.at("log_dir").get_to(config.log_dir);
    
    if (j.contains("buffer_size")) {
        j.at("buffer_size").get_to(config.buffer_size);
    }
    if (j.contains("flush_interval_ms")) {
        j.at("flush_interval_ms").get_to(config.flush_interval_ms);
    }
    if (j.contains("retention_days")) {
        j.at("retention_days").get_to(config.retention_days);
    }
    if (j.contains("compress_old_logs")) {
        j.at("compress_old_logs").get_to(config.compress_old_logs);
    }
    if (j.contains("enable_tick_logging")) {
        j.at("enable_tick_logging").get_to(config.enable_tick_logging);
    }
    if (j.contains("enable_order_logging")) {
        j.at("enable_order_logging").get_to(config.enable_order_logging);
    }
    if (j.contains("enable_fill_logging")) {
        j.at("enable_fill_logging").get_to(config.enable_fill_logging);
    }
    if (j.contains("enable_snapshot_logging")) {
        j.at("enable_snapshot_logging").get_to(config.enable_snapshot_logging);
    }
}

void to_json(json& j, const ZMQConfig& config) {
    j = json{
        {"publish_endpoint", config.publish_endpoint},
        {"high_water_mark", config.high_water_mark},
        {"linger_ms", config.linger_ms}
    };
}

void from_json(const json& j, ZMQConfig& config) {
    j.at("publish_endpoint").get_to(config.publish_endpoint);
    
    if (j.contains("high_water_mark")) {
        j.at("high_water_mark").get_to(config.high_water_mark);
    }
    if (j.contains("linger_ms")) {
        j.at("linger_ms").get_to(config.linger_ms);
    }
}

void to_json(json& j, const MonitoringConfig& config) {
    j = json{
        {"enable_metrics", config.enable_metrics},
        {"metrics_port", config.metrics_port},
        {"enable_health_check", config.enable_health_check},
        {"health_check_port", config.health_check_port},
        {"metrics_update_interval_ms", config.metrics_update_interval_ms}
    };
}

void from_json(const json& j, MonitoringConfig& config) {
    if (j.contains("enable_metrics")) {
        j.at("enable_metrics").get_to(config.enable_metrics);
    }
    if (j.contains("metrics_port")) {
        j.at("metrics_port").get_to(config.metrics_port);
    }
    if (j.contains("enable_health_check")) {
        j.at("enable_health_check").get_to(config.enable_health_check);
    }
    if (j.contains("health_check_port")) {
        j.at("health_check_port").get_to(config.health_check_port);
    }
    if (j.contains("metrics_update_interval_ms")) {
        j.at("metrics_update_interval_ms").get_to(config.metrics_update_interval_ms);
    }
}

void to_json(json& j, const SystemConfig& config) {
    j = json{
        {"exchanges", config.exchanges},
        {"strategies", config.strategies},
        {"risk_parameters", config.risk_parameters},
        {"log_config", config.log_config},
        {"zmq_config", config.zmq_config},
        {"monitoring_config", config.monitoring_config},
        {"initial_capital", config.initial_capital},
        {"state_file", config.state_file}
    };
}

void from_json(const json& j, SystemConfig& config) {
    j.at("exchanges").get_to(config.exchanges);
    j.at("strategies").get_to(config.strategies);
    j.at("risk_parameters").get_to(config.risk_parameters);
    j.at("log_config").get_to(config.log_config);
    
    if (j.contains("zmq_config")) {
        j.at("zmq_config").get_to(config.zmq_config);
    }
    if (j.contains("monitoring_config")) {
        j.at("monitoring_config").get_to(config.monitoring_config);
    }
    if (j.contains("initial_capital")) {
        j.at("initial_capital").get_to(config.initial_capital);
    }
    if (j.contains("state_file")) {
        j.at("state_file").get_to(config.state_file);
    }
}

} // namespace chronos
