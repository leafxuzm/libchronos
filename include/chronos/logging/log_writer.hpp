#pragma once

#include "chronos/core/types.hpp"
#include "chronos/core/config.hpp"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace chronos {
namespace logging {

// ============================================================================
// LogWriter — binary log writer with background flush and daily rotation
// ============================================================================
//
// Fixed-size binary records for O(1) seeking:
//   Tick         64 bytes
//   OrderRequest 128 bytes
//   Fill         128 bytes
//
// File format per type:
//   [header 24B] [record N*sizeof(T)] ...
//   header: magic "CHRONOS\x01" (8B) | version (4B) | log_type (4B) | created_ts (8B)

class LogWriter {
public:
    LogWriter() = default;
    ~LogWriter();

    LogWriter(const LogWriter&) = delete;
    LogWriter& operator=(const LogWriter&) = delete;

    // --- Lifecycle ---

    /// Initialize with log directory and configuration.
    /// Creates the directory if needed, opens initial log files.
    bool initialize(const std::string& log_dir, const LogConfig& config);

    /// Stop the background writer thread and close all files.
    void stop();

    bool isRunning() const { return running_.load(std::memory_order_acquire); }

    // --- Hot path — non-blocking, memcpy into buffer ---

    /// Write a tick record. Returns false if buffer is full.
    bool writeTick(const Tick& tick);

    /// Write an order record. Returns false if buffer is full.
    bool writeOrder(const OrderRequest& order);

    /// Write a fill record. Returns false if buffer is full.
    bool writeFill(const Fill& fill);

    // --- Control ---

    /// Force-flush all buffers to disk immediately.
    void flush();

    /// Rotate log files now (useful for testing).
    void rotateNow();

    // --- Statistics ---

    uint64_t ticksWritten()  const { return ticks_written_.load(std::memory_order_relaxed); }
    uint64_t ordersWritten() const { return orders_written_.load(std::memory_order_relaxed); }
    uint64_t fillsWritten()  const { return fills_written_.load(std::memory_order_relaxed); }

private:
    void run();
    void flushBuffer();

    struct LogBuffer {
        std::vector<uint8_t> data;
        size_t write_pos{0};
        mutable std::mutex mtx;
    };

    // Hot-path append: copies record, returns false if buffer full
    template<typename T>
    bool appendToBuffer(LogBuffer& buf, const T& record);

    // File management
    FILE* openLogFile(const std::string& type, const std::string& date);
    void  writeFileHeader(FILE* f, uint32_t log_type);
    void  closeAllFiles();
    void  rotateFiles(const std::string& today);
    void  deleteOldFiles();

    // --- Configuration ---
    std::string log_dir_;
    LogConfig   config_;

    // --- Buffers ---
    LogBuffer tick_buf_;
    LogBuffer order_buf_;
    LogBuffer fill_buf_;

    // --- Background writer ---
    std::thread         writer_thread_;
    std::atomic<bool>   running_{false};

    // --- Files ---
    std::string current_date_;
    FILE* tick_file_{nullptr};
    FILE* order_file_{nullptr};
    FILE* fill_file_{nullptr};

    // --- Statistics ---
    std::atomic<uint64_t> ticks_written_{0};
    std::atomic<uint64_t> orders_written_{0};
    std::atomic<uint64_t> fills_written_{0};
};

// ============================================================================
// Template implementation (header-only for template)
// ============================================================================

template<typename T>
bool LogWriter::appendToBuffer(LogBuffer& buf, const T& record) {
    constexpr size_t record_size = sizeof(T);
    std::lock_guard<std::mutex> lk(buf.mtx);
    if (buf.write_pos + record_size > buf.data.size()) {
        return false;  // Buffer full — caller should retry or drop
    }
    std::memcpy(buf.data.data() + buf.write_pos, &record, record_size);
    buf.write_pos += record_size;
    return true;
}

}  // namespace logging
}  // namespace chronos
