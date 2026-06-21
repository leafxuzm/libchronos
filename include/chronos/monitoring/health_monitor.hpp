#pragma once

#include <atomic>
#include <cstdint>
#include <shared_mutex>
#include <string>
#include <vector>

namespace chronos {
namespace monitoring {

/// Lightweight in-process health monitor + metrics tracker.
///
/// Thread-safe: metrics are atomic, component health uses shared_mutex.
/// No external HTTP dependency — produces JSON consumable by any HTTP server.
class HealthMonitor {
public:
    enum class Status { HEALTHY, DEGRADED, UNHEALTHY };

    struct ComponentInfo {
        std::string name;
        Status status{Status::HEALTHY};
        std::string message;
        uint64_t last_heartbeat_us{0};
    };

    struct Report {
        bool overall_healthy{true};
        std::vector<ComponentInfo> components;
        uint64_t timestamp_us{0};

        // Cumulative metrics
        uint64_t ticks_total{0};
        uint64_t orders_total{0};
        uint64_t fills_total{0};

        // Instantaneous rates (since last report)
        double ticks_per_sec{0.0};
        double orders_per_sec{0.0};
    };

    HealthMonitor();
    ~HealthMonitor() = default;
    HealthMonitor(const HealthMonitor&) = delete;
    HealthMonitor& operator=(const HealthMonitor&) = delete;

    /// Register a component for health tracking.
    void registerComponent(const std::string& name);

    /// Update heartbeat timestamp for a component.
    void heartbeat(const std::string& name);

    /// Set health status for a component.
    void setStatus(const std::string& name, Status s,
                   const std::string& message = "");

    /// Record a tick, order, or fill for rate calculation.
    void recordTick()   { ticks_.fetch_add(1, std::memory_order_relaxed); }
    void recordOrder()  { orders_.fetch_add(1, std::memory_order_relaxed); }
    void recordFill()   { fills_.fetch_add(1, std::memory_order_relaxed); }

    /// Generate a full health report (resets rate counters).
    Report getReport();

    /// Export report as JSON string.
    std::string toJson();

    /// Read-only snapshot (does not reset rates).
    Report snapshot() const;

    /// Quick overall health check (true = all HEALTHY).
    bool isHealthy() const;

private:
    mutable std::shared_mutex mutex_;

    std::vector<ComponentInfo> components_;

    // Cumulative counters
    std::atomic<uint64_t> ticks_{0};
    std::atomic<uint64_t> orders_{0};
    std::atomic<uint64_t> fills_{0};

    // Rate tracking (reset on each getReport)
    uint64_t last_report_us_{0};
    uint64_t prev_ticks_{0};
    uint64_t prev_orders_{0};
};

}  // namespace monitoring
}  // namespace chronos
