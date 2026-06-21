#include "chronos/logging/log_writer.hpp"
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace chronos {
namespace logging {

namespace {

constexpr uint64_t LOG_MAGIC   = 0x01304E4F524843ULL;  // "CHRONOS\x01"
constexpr uint32_t LOG_VERSION = 1;
constexpr uint32_t LOG_TYPE_TICK  = 0;
constexpr uint32_t LOG_TYPE_ORDER = 1;
constexpr uint32_t LOG_TYPE_FILL  = 2;
constexpr uint32_t LOG_TYPE_SNAP  = 3;
constexpr size_t   HEADER_SIZE = 64;  // padded to alignas(Tick)=64 for mmap record alignment

std::string todayStr() {
    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&tt, &tm);
    std::ostringstream oss;
    oss << std::setfill('0')
        << std::setw(4) << (tm.tm_year + 1900)
        << std::setw(2) << (tm.tm_mon + 1)
        << std::setw(2) << tm.tm_mday;
    return oss.str();
}

uint64_t nowUs() {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

}  // namespace

// ============================================================================
// Destructor
// ============================================================================

LogWriter::~LogWriter() {
    stop();
}

// ============================================================================
// Initialize
// ============================================================================

bool LogWriter::initialize(const std::string& log_dir, const LogConfig& config) {
    log_dir_ = log_dir;
    config_  = config;

    // Create log directory
    std::error_code ec;
    std::filesystem::create_directories(log_dir_, ec);
    if (ec) return false;

    // Allocate buffers
    size_t buf_bytes = config_.buffer_size;
    tick_buf_.data.resize(buf_bytes);
    order_buf_.data.resize(buf_bytes);
    fill_buf_.data.resize(buf_bytes);

    // Open initial files
    current_date_ = todayStr();
    tick_file_  = openLogFile("tick",  current_date_);
    order_file_ = openLogFile("order", current_date_);
    fill_file_  = openLogFile("fill",  current_date_);

    if (!tick_file_ || !order_file_ || !fill_file_) {
        closeAllFiles();
        return false;
    }

    // Delete old files
    deleteOldFiles();

    // Start background writer
    running_.store(true, std::memory_order_release);
    writer_thread_ = std::thread(&LogWriter::run, this);

    return true;
}

void LogWriter::stop() {
    if (!running_.load(std::memory_order_acquire)) return;
    running_.store(false, std::memory_order_release);
    if (writer_thread_.joinable()) {
        writer_thread_.join();
    }
    flush();  // Final flush
    closeAllFiles();
}

// ============================================================================
// Hot path
// ============================================================================

bool LogWriter::writeTick(const Tick& tick) {
    if (!appendToBuffer(tick_buf_, tick)) return false;
    ticks_written_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool LogWriter::writeOrder(const OrderRequest& order) {
    if (!appendToBuffer(order_buf_, order)) return false;
    orders_written_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool LogWriter::writeFill(const Fill& fill) {
    if (!appendToBuffer(fill_buf_, fill)) return false;
    fills_written_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ============================================================================
// Background writer
// ============================================================================

void LogWriter::run() {
    while (running_.load(std::memory_order_acquire)) {
        // Periodic flush (~100ms)
        std::this_thread::sleep_for(
            std::chrono::milliseconds(config_.flush_interval_ms));

        // Check date rotation
        std::string today = todayStr();
        if (today != current_date_) {
            rotateFiles(today);
        }

        flushBuffer();
    }
}

// ============================================================================
// Flush
// ============================================================================

void LogWriter::flush() {
    flushBuffer();
}

void LogWriter::flushBuffer() {
    // Swap-and-write for each buffer: copy data under lock, then write unlocked
    {
        std::lock_guard<std::mutex> lk(tick_buf_.mtx);
        if (tick_buf_.write_pos > 0 && tick_file_) {
            size_t n = tick_buf_.write_pos;
            fwrite(tick_buf_.data.data(), 1, n, tick_file_);
            fflush(tick_file_);
            tick_buf_.write_pos = 0;
        }
    }
    {
        std::lock_guard<std::mutex> lk(order_buf_.mtx);
        if (order_buf_.write_pos > 0 && order_file_) {
            size_t n = order_buf_.write_pos;
            fwrite(order_buf_.data.data(), 1, n, order_file_);
            fflush(order_file_);
            order_buf_.write_pos = 0;
        }
    }
    {
        std::lock_guard<std::mutex> lk(fill_buf_.mtx);
        if (fill_buf_.write_pos > 0 && fill_file_) {
            size_t n = fill_buf_.write_pos;
            fwrite(fill_buf_.data.data(), 1, n, fill_file_);
            fflush(fill_file_);
            fill_buf_.write_pos = 0;
        }
    }
}

// ============================================================================
// Rotation
// ============================================================================

void LogWriter::rotateNow() {
    std::string today = todayStr();
    if (today != current_date_) {
        rotateFiles(today);
    }
}

void LogWriter::rotateFiles(const std::string& today) {
    flush();  // Drain everything to current files first
    closeAllFiles();

    current_date_ = today;
    tick_file_  = openLogFile("tick",  today);
    order_file_ = openLogFile("order", today);
    fill_file_  = openLogFile("fill",  today);

    deleteOldFiles();
}

// ============================================================================
// File management
// ============================================================================

FILE* LogWriter::openLogFile(const std::string& type, const std::string& date) {
    std::string path = log_dir_ + "/" + type + "_" + date + ".bin";
    FILE* f = fopen(path.c_str(), "ab");
    if (!f) return nullptr;

    // Write header if file is new
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz == 0) {
        uint32_t ltype = LOG_TYPE_TICK;
        if (type == "order") ltype = LOG_TYPE_ORDER;
        else if (type == "fill") ltype = LOG_TYPE_FILL;
        else if (type == "snap") ltype = LOG_TYPE_SNAP;
        writeFileHeader(f, ltype);
    }
    return f;
}

void LogWriter::writeFileHeader(FILE* f, uint32_t log_type) {
    uint8_t header[HEADER_SIZE] = {};
    // Magic: "CHRONOS\x01"
    std::memcpy(header,     &LOG_MAGIC,   8);
    std::memcpy(header + 8,  &LOG_VERSION, 4);
    std::memcpy(header + 12, &log_type,    4);
    uint64_t ts = nowUs();
    std::memcpy(header + 16, &ts,          8);
    fwrite(header, 1, HEADER_SIZE, f);
}

void LogWriter::closeAllFiles() {
    if (tick_file_)  { fclose(tick_file_);  tick_file_  = nullptr; }
    if (order_file_) { fclose(order_file_); order_file_ = nullptr; }
    if (fill_file_)  { fclose(fill_file_);  fill_file_  = nullptr; }
}

void LogWriter::deleteOldFiles() {
    if (config_.retention_days == 0) return;

    auto cutoff = std::chrono::system_clock::now()
                - std::chrono::hours(24 * config_.retention_days);
    auto cutoff_tt = std::chrono::system_clock::to_time_t(cutoff);

    std::error_code ec;
    for (auto& entry : std::filesystem::directory_iterator(log_dir_, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        // Parse date from filename: type_YYYYMMDD.bin
        std::string fname = entry.path().filename().string();
        if (fname.size() < 13) continue;
        // Date is 8 chars before ".bin"
        std::string date_str = fname.substr(fname.size() - 12, 8);
        if (date_str < current_date_) {
            // Only delete our own log files (by pattern)
            if (fname.find("tick_") == 0 || fname.find("order_") == 0 ||
                fname.find("fill_") == 0) {
                std::filesystem::remove(entry.path(), ec);
            }
        }
    }
}

}  // namespace logging
}  // namespace chronos
