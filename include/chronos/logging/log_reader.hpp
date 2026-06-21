#pragma once

#include "chronos/core/types.hpp"
#include <cstddef>
#include <cstdint>
#include <string>

namespace chronos {
namespace logging {

// ============================================================================
// LogFileHeader — 24-byte header at the start of every binary log file
// ============================================================================

struct LogFileHeader {
    uint64_t magic;                  // "CHRONOS\x01" = 0x01304E4F524843
    uint32_t version;                // currently 1
    uint32_t log_type;               // 0=TICK, 1=ORDER, 2=FILL, 3=SNAP
    uint64_t created_timestamp_us;   // when the file was created
};

// ============================================================================
// LogReader — mmap-based zero-copy binary log reader
// ============================================================================
//
// Maps a binary log file into memory and provides typed, zero-copy access
// to fixed-size records. Supports timestamp-based seeking via binary search.
//
// One LogReader = one file. For multi-file access use LogFileSet.

class LogReader {
public:
    LogReader() = default;
    ~LogReader();

    // Movable, not copyable (owns fd + mmap region)
    LogReader(const LogReader&) = delete;
    LogReader& operator=(const LogReader&) = delete;
    LogReader(LogReader&& other) noexcept;
    LogReader& operator=(LogReader&& other) noexcept;

    // --- Open / Close ---

    /// Open and mmap a binary log file. Returns false if the file
    /// doesn't exist, is too small, or has a corrupted header.
    bool open(const std::string& filepath);

    /// Release the mmap region and close the file descriptor.
    void close();

    bool isOpen() const { return mapped_data_ != nullptr; }

    // --- Header ---

    const LogFileHeader& header() const { return header_; }
    uint32_t logType() const { return header_.log_type; }
    uint64_t createdTimestamp() const { return header_.created_timestamp_us; }
    const std::string& filename() const { return filename_; }

    // --- Record Info ---

    /// Number of complete records in the file.
    size_t recordCount() const { return record_count_; }

    /// Size of each record in bytes (64 for Tick, 128 for Order/Fill).
    size_t recordSize() const { return record_size_; }

    // --- Zero-Copy Access ---

    /// Raw pointer to the mapped record at |index|. Returns nullptr if out of bounds.
    const void* recordAt(size_t index) const;

    /// Typed accessors. Return nullptr if the log type doesn't match or index
    /// is out of bounds. The returned pointer points directly into the mmap'd
    /// region — no copy is made.
    const Tick*         tickAt(size_t index)  const;
    const OrderRequest* orderAt(size_t index) const;
    const Fill*         fillAt(size_t index)  const;

    // --- Seeking ---

    /// Return the timestamp of the record at |index|.
    /// The timestamp field used depends on log type (exchange_timestamp_us for
    /// Tick/Fill, timestamp_us for OrderRequest).
    uint64_t timestampAt(size_t index) const;

    /// Binary search for the first record whose timestamp >= |timestamp_us|.
    /// Returns recordCount() if all records have smaller timestamps.
    size_t seekToTimestamp(uint64_t timestamp_us) const;

    // --- Validation ---

    /// Basic sanity check on a single record. Writes a description to |error|
    /// if the record looks suspicious. Returns true if the record passes.
    bool validateRecord(size_t index, std::string& error) const;

private:
    void moveFrom(LogReader&& other) noexcept;
    void reset();

    int         fd_{-1};
    void*       mapped_data_{nullptr};
    size_t      file_size_{0};
    size_t      record_size_{0};
    size_t      record_count_{0};
    size_t      timestamp_offset_{0};  // offset of timestamp field within record
    LogFileHeader header_{};
    std::string filename_;
};

// ============================================================================
// LogFileSet — convenience for opening tick/order/fill logs from a directory
// ============================================================================

struct LogFileSet {
    LogReader tick;
    LogReader order;
    LogReader fill;
};

/// Open all log files in |dir| for the given |date| (YYYYMMDD, or empty for
/// today). Files are identified by their prefix: tick_, order_, fill_.
LogFileSet openLogDirectory(const std::string& dir, const std::string& date = "");

}  // namespace logging
}  // namespace chronos
