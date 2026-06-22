#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include <chrono>

namespace chronos {
namespace security {

// ============================================================================
// API Key Encryption — XOR-based with master key from environment
// ============================================================================

/// Encrypts/decrypts API keys so they aren't stored in plaintext.
///
/// Uses a simple XOR cipher with a key derived from the CHRONOS_MASTER_KEY
/// environment variable. Not cryptographically strong, but prevents casual
/// exposure of API keys in config files and logs.
///
/// The encrypted output is base64-encoded for safe storage in JSON config.
class ApiKeyEncryptor {
public:
    ApiKeyEncryptor();

    /// Encrypt a plaintext API key. Returns base64-encoded ciphertext.
    std::string encrypt(const std::string& plaintext) const;

    /// Decrypt a base64-encoded ciphertext back to plaintext.
    std::string decrypt(const std::string& ciphertext_b64) const;

    /// Check if a master key is configured.
    bool isConfigured() const { return !master_key_.empty(); }

private:
    std::vector<uint8_t> master_key_;
};

// ============================================================================
// Log Sanitizer — redacts sensitive information from log output
// ============================================================================

/// Redacts sensitive patterns from log strings before writing or transmitting.
///
/// Detects and masks:
///   - API keys / secrets (hex strings > 32 chars)
///   - Private keys (PEM headers)
///   - Token-like strings (base64 > 40 chars)
class LogSanitizer {
public:
    struct Config {
        bool mask_api_keys;
        bool mask_private_keys;
        bool mask_tokens;
        char mask_char;
        Config() : mask_api_keys(true), mask_private_keys(true),
                   mask_tokens(true), mask_char('*') {}
    };

    LogSanitizer() : config_() {}
    explicit LogSanitizer(const Config& cfg) : config_(cfg) {}

    /// Sanitize a single log line. Returns sanitized copy.
    std::string sanitize(const std::string& input) const;

    /// Check if a string contains sensitive data that should be masked.
    bool containsSensitiveData(const std::string& input) const;

private:
    Config config_;

    static std::string maskApiKeys(const std::string& input, char mask);
    static std::string maskPrivateKeys(const std::string& input, char mask);
    static std::string maskTokens(const std::string& input, char mask);
};

// ============================================================================
// Token Validator — constant-time token comparison + rate limiting
// ============================================================================

/// Validates API tokens for HTTP endpoints.
///
/// Uses constant-time comparison to prevent timing attacks.
/// Integrated rate limiter rejects after max_requests_per_second.
class TokenValidator {
public:
    struct Config {
        std::string valid_token;          // Expected token (SHA256 recommended)
        uint32_t max_requests_per_second{10};
    };

    explicit TokenValidator(const Config& cfg) : config_(cfg) {}

    /// Validate a token. Returns true if token matches AND rate limit not exceeded.
    /// Constant-time comparison prevents timing attacks.
    bool validate(const std::string& token);

    /// Check rate limit only (does not validate token).
    bool checkRateLimit();

    /// Reset the rate limiter for the current second.
    void resetRateLimit();

    uint64_t accepted_count() const { return accepted_.load(std::memory_order_relaxed); }
    uint64_t rejected_count() const { return rejected_.load(std::memory_order_relaxed); }

private:
    Config config_;
    std::atomic<uint32_t> current_second_{0};
    std::atomic<uint32_t> requests_this_second_{0};
    std::atomic<uint64_t> accepted_{0};
    std::atomic<uint64_t> rejected_{0};
};

}  // namespace security
}  // namespace chronos
