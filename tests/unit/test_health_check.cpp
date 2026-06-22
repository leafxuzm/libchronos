/**
 * @file test_health_check.cpp
 * @brief Unit tests for HealthMonitor — component tracking, metrics, JSON export
 *
 * Validates: Requirements 24.1-24.6 (health checks, metrics export)
 */

#include <gtest/gtest.h>
#include <chronos/monitoring/health_monitor.hpp>
#include <nlohmann/json.hpp>
#include <thread>

using namespace chronos::monitoring;

// ============================================================================
// 1. Component Registration & Status
// ============================================================================

TEST(HealthMonitorTest, RegisterAndQueryComponents) {
    HealthMonitor monitor;

    monitor.registerComponent("strategy_engine");
    monitor.registerComponent("order_gateway");
    monitor.registerComponent("market_data");

    auto report = monitor.snapshot();
    EXPECT_EQ(report.components.size(), 3u);

    // All should start HEALTHY
    for (auto& c : report.components) {
        EXPECT_EQ(c.status, HealthMonitor::Status::HEALTHY);
        EXPECT_GT(c.last_heartbeat_us, 0u);
    }
}

TEST(HealthMonitorTest, DuplicateRegistrationIsIgnored) {
    HealthMonitor monitor;
    monitor.registerComponent("engine");
    monitor.registerComponent("engine");  // duplicate
    monitor.registerComponent("engine");  // duplicate

    auto report = monitor.snapshot();
    EXPECT_EQ(report.components.size(), 1u);
}

TEST(HealthMonitorTest, SetComponentStatus) {
    HealthMonitor monitor;
    monitor.registerComponent("engine");
    monitor.registerComponent("gateway");

    monitor.setStatus("engine", HealthMonitor::Status::DEGRADED,
                      "High latency detected");
    monitor.setStatus("gateway", HealthMonitor::Status::UNHEALTHY,
                      "Connection lost");

    auto report = monitor.snapshot();
    EXPECT_FALSE(report.overall_healthy);

    auto* engine = &report.components[0];
    auto* gateway = &report.components[1];
    if (engine->name != "engine") std::swap(engine, gateway);

    EXPECT_EQ(engine->status, HealthMonitor::Status::DEGRADED);
    EXPECT_EQ(engine->message, "High latency detected");
    EXPECT_EQ(gateway->status, HealthMonitor::Status::UNHEALTHY);
    EXPECT_EQ(gateway->message, "Connection lost");
}

TEST(HealthMonitorTest, HeartbeatUpdatesTimestamp) {
    HealthMonitor monitor;
    monitor.registerComponent("engine");

    auto snap1 = monitor.snapshot();
    uint64_t ts1 = snap1.components[0].last_heartbeat_us;

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    monitor.heartbeat("engine");
    auto snap2 = monitor.snapshot();
    uint64_t ts2 = snap2.components[0].last_heartbeat_us;

    EXPECT_GT(ts2, ts1) << "Heartbeat should update timestamp";
}

TEST(HealthMonitorTest, IsHealthyQuickCheck) {
    HealthMonitor monitor;
    monitor.registerComponent("a");
    monitor.registerComponent("b");

    EXPECT_TRUE(monitor.isHealthy());

    monitor.setStatus("a", HealthMonitor::Status::DEGRADED);
    EXPECT_FALSE(monitor.isHealthy());

    monitor.setStatus("a", HealthMonitor::Status::HEALTHY);
    EXPECT_TRUE(monitor.isHealthy());
}

// ============================================================================
// 2. Metrics Tracking
// ============================================================================

TEST(HealthMonitorTest, RecordsMetrics) {
    HealthMonitor monitor;

    for (int i = 0; i < 1000; ++i) monitor.recordTick();
    for (int i = 0; i < 500; ++i)  monitor.recordOrder();
    for (int i = 0; i < 100; ++i)  monitor.recordFill();

    auto report = monitor.getReport();
    EXPECT_EQ(report.ticks_total, 1000u);
    EXPECT_EQ(report.orders_total, 500u);
    EXPECT_EQ(report.fills_total, 100u);
}

TEST(HealthMonitorTest, RateCalculation) {
    HealthMonitor monitor;

    // Record 1000 ticks
    for (int i = 0; i < 1000; ++i) monitor.recordTick();
    for (int i = 0; i < 100;  ++i) monitor.recordOrder();

    // First report establishes baseline (rate should be near 0 or based on very short interval)
    auto r1 = monitor.getReport();
    EXPECT_EQ(r1.ticks_total, 1000u);

    // Add more and wait
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    for (int i = 0; i < 500; ++i) monitor.recordTick();
    for (int i = 0; i < 50;  ++i) monitor.recordOrder();

    auto r2 = monitor.getReport();
    EXPECT_EQ(r2.ticks_total, 1500u);
    EXPECT_GT(r2.ticks_per_sec, 0.0) << "Rate should be positive after second report";
    EXPECT_GT(r2.orders_per_sec, 0.0);
}

TEST(HealthMonitorTest, SnapshotDoesNotResetRates) {
    HealthMonitor monitor;
    for (int i = 0; i < 100; ++i) monitor.recordTick();

    auto snap1 = monitor.snapshot();
    auto snap2 = monitor.snapshot();
    EXPECT_EQ(snap1.ticks_total, snap2.ticks_total);

    auto report = monitor.getReport();
    EXPECT_EQ(report.ticks_total, 100u);
}

// ============================================================================
// 3. JSON Export
// ============================================================================

TEST(HealthMonitorTest, ExportToJson) {
    HealthMonitor monitor;
    monitor.registerComponent("strategy_engine");
    monitor.registerComponent("risk_engine");

    monitor.recordTick();
    monitor.recordTick();
    monitor.recordOrder();

    std::string json_str = monitor.toJson();
    ASSERT_FALSE(json_str.empty());

    auto j = nlohmann::json::parse(json_str);

    EXPECT_TRUE(j.contains("healthy"));
    EXPECT_TRUE(j.contains("timestamp_us"));
    EXPECT_TRUE(j.contains("metrics"));
    EXPECT_TRUE(j.contains("components"));

    EXPECT_TRUE(j["healthy"].get<bool>());
    EXPECT_EQ(j["metrics"]["ticks_total"].get<uint64_t>(), 2u);
    EXPECT_EQ(j["metrics"]["orders_total"].get<uint64_t>(), 1u);
    EXPECT_EQ(j["components"].size(), 2u);

    // Verify component fields
    for (auto& c : j["components"]) {
        EXPECT_TRUE(c.contains("name"));
        EXPECT_TRUE(c.contains("status"));
        EXPECT_TRUE(c.contains("message"));
        EXPECT_TRUE(c.contains("last_heartbeat_us"));
        EXPECT_EQ(c["status"].get<std::string>(), "HEALTHY");
    }
}

TEST(HealthMonitorTest, JsonReflectsUnhealthyState) {
    HealthMonitor monitor;
    monitor.registerComponent("gateway");
    monitor.setStatus("gateway", HealthMonitor::Status::UNHEALTHY, "Down");

    std::string json_str = monitor.toJson();
    auto j = nlohmann::json::parse(json_str);

    EXPECT_FALSE(j["healthy"].get<bool>());
    EXPECT_EQ(j["components"][0]["status"].get<std::string>(), "UNHEALTHY");
    EXPECT_EQ(j["components"][0]["message"].get<std::string>(), "Down");
}

// ============================================================================
// 4. Thread Safety (basic concurrent access)
// ============================================================================

TEST(HealthMonitorTest, ConcurrentMetricsRecording) {
    HealthMonitor monitor;
    monitor.registerComponent("engine");

    constexpr int THREADS = 4;
    constexpr int PER_THREAD = 25000;
    std::vector<std::thread> workers;

    for (int t = 0; t < THREADS; ++t) {
        workers.emplace_back([&monitor]() {
            for (int i = 0; i < PER_THREAD; ++i) {
                monitor.recordTick();
                if (i % 10 == 0) monitor.recordOrder();
            }
        });
    }

    for (auto& w : workers) w.join();

    auto report = monitor.getReport();
    EXPECT_EQ(report.ticks_total, THREADS * PER_THREAD);
    EXPECT_EQ(report.orders_total, THREADS * PER_THREAD / 10);
    EXPECT_TRUE(monitor.isHealthy());
}

TEST(HealthMonitorTest, ConcurrentStatusUpdates) {
    HealthMonitor monitor;
    monitor.registerComponent("comp_a");
    monitor.registerComponent("comp_b");

    std::atomic<bool> start{false};

    std::thread t1([&]() {
        while (!start.load(std::memory_order_relaxed)) {}
        for (int i = 0; i < 100; ++i) {
            auto s = (i % 2 == 0) ? HealthMonitor::Status::HEALTHY
                                  : HealthMonitor::Status::DEGRADED;
            monitor.setStatus("comp_a", s);
        }
    });
    std::thread t2([&]() {
        while (!start.load(std::memory_order_relaxed)) {}
        for (int i = 0; i < 100; ++i) {
            monitor.heartbeat("comp_b");
            auto snap = monitor.snapshot();
            EXPECT_GE(snap.components.size(), 2u);
        }
    });

    start.store(true, std::memory_order_release);
    t1.join();
    t2.join();

    // Should not crash, components should still exist
    auto report = monitor.snapshot();
    EXPECT_EQ(report.components.size(), 2u);
}
