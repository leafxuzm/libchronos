#include "chronos/logging/log_reader.hpp"
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace chronos {
namespace logging {

namespace {

constexpr uint64_t LOG_MAGIC   = 0x01304E4F524843ULL;  // "CHRONOS\x01"
constexpr uint32_t LOG_VERSION = 1;
constexpr size_t   HEADER_SIZE = 24;

// Record size by log type
size_t recordSizeForType(uint32_t log_type) {
    switch (log_type) {
        case 0: return sizeof(Tick);          // 64
        case 1: return sizeof(OrderRequest);  // 128
        case 2: return sizeof(Fill);          // 128
        default: return 0;
    }
}

// Timestamp offset within record by log type
size_t timestampOffsetForType(uint32_t log_type) {
    switch (log_type) {
        case 0: return offsetof(Tick, exchange_timestamp_us);          // 0
        case 1: return offsetof(OrderRequest, timestamp_us);           // 8
        case 2: return offsetof(Fill, exchange_timestamp_us);          // 24
        default: return 0;
    }
}

std::string todayStr() {
    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&tt, &tm);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

}  // namespace

// ============================================================================
// Destructor & Move
// ============================================================================

LogReader::~LogReader() {
    close();
}

LogReader::LogReader(LogReader&& other) noexcept {
    moveFrom(std::move(other));
}

LogReader& LogReader::operator=(LogReader&& other) noexcept {
    if (this != &other) {
        close();
        moveFrom(std::move(other));
    }
    return *this;
}

void LogReader::moveFrom(LogReader&& other) noexcept {
    fd_              = other.fd_;
    mapped_data_     = other.mapped_data_;
    file_size_       = other.file_size_;
    record_size_     = other.record_size_;
    record_count_    = other.record_count_;
    timestamp_offset_ = other.timestamp_offset_;
    header_          = other.header_;
    filename_        = std::move(other.filename_);
    other.reset();
}

void LogReader::reset() {
    fd_ = -1;
    mapped_data_ = nullptr;
    file_size_ = 0;
    record_size_ = 0;
    record_count_ = 0;
    timestamp_offset_ = 0;
    header_ = {};
}

// ============================================================================
// Open / Close
// ============================================================================

bool LogReader::open(const std::string& filepath) {
    close();

    fd_ = ::open(filepath.c_str(), O_RDONLY);
    if (fd_ < 0) return false;

    // Get file size
    struct stat st{};
    if (::fstat(fd_, &st) != 0) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    file_size_ = static_cast<size_t>(st.st_size);

    // File must be at least header size
    if (file_size_ < HEADER_SIZE) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    // mmap the whole file read-only
    mapped_data_ = ::mmap(nullptr, file_size_, PROT_READ, MAP_PRIVATE, fd_, 0);
    if (mapped_data_ == MAP_FAILED) {
        ::close(fd_);
        fd_ = -1;
        mapped_data_ = nullptr;
        return false;
    }

    // Parse header
    auto* raw = static_cast<const uint8_t*>(mapped_data_);
    std::memcpy(&header_.magic,                raw,      8);
    std::memcpy(&header_.version,              raw + 8,  4);
    std::memcpy(&header_.log_type,             raw + 12, 4);
    std::memcpy(&header_.created_timestamp_us, raw + 16, 8);

    // Validate magic
    if (header_.magic != LOG_MAGIC) {
        close();
        return false;
    }

    // Determine record size and timestamp offset
    record_size_ = recordSizeForType(header_.log_type);
    timestamp_offset_ = timestampOffsetForType(header_.log_type);

    if (record_size_ == 0) {
        close();
        return false;
    }

    // Calculate record count (truncate trailing partial record)
    size_t data_size = file_size_ - HEADER_SIZE;
    record_count_ = data_size / record_size_;

    filename_ = filepath;
    return true;
}

void LogReader::close() {
    if (mapped_data_) {
        ::munmap(mapped_data_, file_size_);
        mapped_data_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    reset();
}

// ============================================================================
// Record Access
// ============================================================================

const void* LogReader::recordAt(size_t index) const {
    if (index >= record_count_) return nullptr;
    auto* base = static_cast<const uint8_t*>(mapped_data_);
    return base + HEADER_SIZE + index * record_size_;
}

const Tick* LogReader::tickAt(size_t index) const {
    if (header_.log_type != 0) return nullptr;
    return static_cast<const Tick*>(recordAt(index));
}

const OrderRequest* LogReader::orderAt(size_t index) const {
    if (header_.log_type != 1) return nullptr;
    return static_cast<const OrderRequest*>(recordAt(index));
}

const Fill* LogReader::fillAt(size_t index) const {
    if (header_.log_type != 2) return nullptr;
    return static_cast<const Fill*>(recordAt(index));
}

// ============================================================================
// Seeking
// ============================================================================

uint64_t LogReader::timestampAt(size_t index) const {
    auto* rec = static_cast<const uint8_t*>(recordAt(index));
    if (!rec) return 0;
    uint64_t ts = 0;
    std::memcpy(&ts, rec + timestamp_offset_, sizeof(uint64_t));
    return ts;
}

size_t LogReader::seekToTimestamp(uint64_t timestamp_us) const {
    if (record_count_ == 0) return 0;
    if (timestampAt(record_count_ - 1) < timestamp_us) return record_count_;

    size_t lo = 0, hi = record_count_;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (timestampAt(mid) < timestamp_us) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

// ============================================================================
// Validation
// ============================================================================

bool LogReader::validateRecord(size_t index, std::string& error) const {
    auto* rec = static_cast<const uint8_t*>(recordAt(index));
    if (!rec) {
        error = "index out of bounds";
        return false;
    }

    uint64_t ts = 0;
    std::memcpy(&ts, rec + timestamp_offset_, sizeof(uint64_t));
    if (ts == 0) {
        error = "timestamp is zero";
        return false;
    }

    // Rough sanity: timestamp before year 2100 (4102444800000000 us since epoch)
    if (ts > 4102444800000000ULL) {
        error = "timestamp unreasonably large";
        return false;
    }

    switch (header_.log_type) {
        case 0: {  // Tick
            auto* t = reinterpret_cast<const Tick*>(rec);
            // Price and quantity should be non-negative (Decimal raw_value >= 0)
            if (t->price.raw_value() < 0 || t->quantity.raw_value() < 0) {
                error = "negative price or quantity in tick";
                return false;
            }
            if (static_cast<uint8_t>(t->side) > 2) {
                error = "invalid tick side";
                return false;
            }
            break;
        }
        case 1: {  // OrderRequest
            auto* o = reinterpret_cast<const OrderRequest*>(rec);
            if (o->order_id == 0) {
                error = "order_id is zero";
                return false;
            }
            if (static_cast<uint8_t>(o->side) > 1) {
                error = "invalid order side";
                return false;
            }
            break;
        }
        case 2: {  // Fill
            auto* f = reinterpret_cast<const Fill*>(rec);
            if (f->order_id == 0) {
                error = "fill order_id is zero";
                return false;
            }
            if (f->fill_price.raw_value() < 0 || f->fill_quantity.raw_value() < 0) {
                error = "negative price or quantity in fill";
                return false;
            }
            break;
        }
    }

    return true;
}

// ============================================================================
// LogFileSet
// ============================================================================

LogFileSet openLogDirectory(const std::string& dir, const std::string& date) {
    LogFileSet set;
    std::string d = date.empty() ? todayStr() : date;

    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return set;

    set.tick.open(dir + "/tick_" + d + ".bin");
    set.order.open(dir + "/order_" + d + ".bin");
    set.fill.open(dir + "/fill_" + d + ".bin");

    return set;
}

}  // namespace logging
}  // namespace chronos
