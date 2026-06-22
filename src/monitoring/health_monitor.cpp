#include "chronos/monitoring/health_monitor.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <mutex>

namespace chronos {
namespace monitoring {

using json = nlohmann::json;

namespace {

uint64_t now_us() {
    auto now = std::chrono::system_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch());
    return static_cast<uint64_t>(us.count());
}

const char* statusStr(HealthMonitor::Status s) {
    switch (s) {
        case HealthMonitor::Status::HEALTHY:   return "HEALTHY";
        case HealthMonitor::Status::DEGRADED:  return "DEGRADED";
        case HealthMonitor::Status::UNHEALTHY: return "UNHEALTHY";
    }
    return "UNKNOWN";
}

} // anonymous namespace

HealthMonitor::HealthMonitor() {
    last_report_us_ = now_us();
}

void HealthMonitor::registerComponent(const std::string& name) {
    std::unique_lock lock(mutex_);
    // Avoid duplicates
    for (auto& c : components_) {
        if (c.name == name) return;
    }
    components_.push_back({name, Status::HEALTHY, "", now_us()});
}

void HealthMonitor::heartbeat(const std::string& name) {
    std::unique_lock lock(mutex_);
    for (auto& c : components_) {
        if (c.name == name) {
            c.last_heartbeat_us = now_us();
            return;
        }
    }
}

void HealthMonitor::setStatus(const std::string& name, Status s,
                               const std::string& message) {
    std::unique_lock lock(mutex_);
    for (auto& c : components_) {
        if (c.name == name) {
            c.status = s;
            c.message = message;
            c.last_heartbeat_us = now_us();
            return;
        }
    }
}

HealthMonitor::Report HealthMonitor::getReport() {
    uint64_t report_ts = now_us();
    std::shared_lock lock(mutex_);

    Report r;
    r.timestamp_us = report_ts;
    r.components = components_;

    bool all_healthy = true;
    for (auto& c : r.components) {
        if (c.status != Status::HEALTHY) all_healthy = false;
    }
    r.overall_healthy = all_healthy;

    // Cumulative counters
    r.ticks_total  = ticks_.load(std::memory_order_relaxed);
    r.orders_total = orders_.load(std::memory_order_relaxed);
    r.fills_total  = fills_.load(std::memory_order_relaxed);

    // Rate calculation (per second)
    uint64_t elapsed_us = report_ts - last_report_us_;
    if (elapsed_us > 0) {
        double elapsed_s = static_cast<double>(elapsed_us) / 1'000'000.0;
        r.ticks_per_sec  = static_cast<double>(r.ticks_total - prev_ticks_) / elapsed_s;
        r.orders_per_sec = static_cast<double>(r.orders_total - prev_orders_) / elapsed_s;
    }

    return r;
}

std::string HealthMonitor::toJson() {
    Report r = getReport();

    json j;
    j["healthy"] = r.overall_healthy;
    j["timestamp_us"] = r.timestamp_us;

    j["metrics"]["ticks_total"]    = r.ticks_total;
    j["metrics"]["orders_total"]   = r.orders_total;
    j["metrics"]["fills_total"]    = r.fills_total;
    j["metrics"]["ticks_per_sec"]  = r.ticks_per_sec;
    j["metrics"]["orders_per_sec"] = r.orders_per_sec;

    auto comps = json::array();
    for (auto& c : r.components) {
        json cj;
        cj["name"] = c.name;
        cj["status"] = statusStr(c.status);
        cj["message"] = c.message;
        cj["last_heartbeat_us"] = c.last_heartbeat_us;
        comps.push_back(cj);
    }
    j["components"] = comps;

    // Update rate tracking
    last_report_us_ = r.timestamp_us;
    prev_ticks_  = r.ticks_total;
    prev_orders_ = r.orders_total;

    return j.dump(2);  // pretty-print with 2-space indent
}

HealthMonitor::Report HealthMonitor::snapshot() const {
    std::shared_lock lock(mutex_);

    Report r;
    r.timestamp_us = now_us();
    r.components = components_;

    bool all_healthy = true;
    for (auto& c : r.components) {
        if (c.status != Status::HEALTHY) all_healthy = false;
    }
    r.overall_healthy = all_healthy;

    r.ticks_total  = ticks_.load(std::memory_order_relaxed);
    r.orders_total = orders_.load(std::memory_order_relaxed);
    r.fills_total  = fills_.load(std::memory_order_relaxed);

    return r;
}

bool HealthMonitor::isHealthy() const {
    std::shared_lock lock(mutex_);
    for (auto& c : components_) {
        if (c.status != Status::HEALTHY) return false;
    }
    return true;
}

}  // namespace monitoring
}  // namespace chronos
