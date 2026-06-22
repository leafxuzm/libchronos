/**
 * @file test_security.cpp
 * @brief Unit tests for security module — API key encryption, log sanitizer, token validator
 *
 * Validates: Requirements 25.1-25.5 (API key encryption, log sanitization, HTTP auth)
 */

#include <gtest/gtest.h>
#include <chronos/io/security/security.hpp>
#include <cstdlib>
#include <thread>
#include <vector>

using namespace chronos::security;

// ============================================================================
// 1. ApiKeyEncryptor — encrypt/decrypt round-trip
// ============================================================================

class ApiKeyEncryptorTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Save original env
        orig_ = std::getenv("CHRONOS_MASTER_KEY");
        if (orig_) orig_val_ = orig_;
    }

    void TearDown() override {
        // Restore original env
        if (!orig_val_.empty()) {
            setenv("CHRONOS_MASTER_KEY", orig_val_.c_str(), 1);
        } else {
            unsetenv("CHRONOS_MASTER_KEY");
        }
    }

    const char* orig_{nullptr};
    std::string orig_val_;
};

TEST_F(ApiKeyEncryptorTest, WithoutMasterKeyReturnsPlaintext) {
    unsetenv("CHRONOS_MASTER_KEY");
    ApiKeyEncryptor encryptor;
    EXPECT_FALSE(encryptor.isConfigured());

    std::string secret = "my-secret-api-key-12345";
    std::string encrypted = encryptor.encrypt(secret);
    EXPECT_EQ(encrypted, secret);  // no key → pass through

    std::string decrypted = encryptor.decrypt(secret);
    EXPECT_EQ(decrypted, secret);
}

TEST_F(ApiKeyEncryptorTest, EncryptDecryptRoundTrip) {
    setenv("CHRONOS_MASTER_KEY", "my-strong-master-key-2024", 1);
    ApiKeyEncryptor encryptor;
    EXPECT_TRUE(encryptor.isConfigured());

    std::string original = "sk-abc123def456ghi789jkl";
    std::string encrypted = encryptor.encrypt(original);
    EXPECT_NE(encrypted, original) << "Encrypted text should differ from plaintext";

    std::string decrypted = encryptor.decrypt(encrypted);
    EXPECT_EQ(decrypted, original) << "Decrypted text should match original";
}

TEST_F(ApiKeyEncryptorTest, DifferentMasterKeysProduceDifferentCiphertext) {
    setenv("CHRONOS_MASTER_KEY", "key-alpha", 1);
    ApiKeyEncryptor e1;
    std::string ct1 = e1.encrypt("test-secret");

    setenv("CHRONOS_MASTER_KEY", "key-beta", 1);
    ApiKeyEncryptor e2;
    std::string ct2 = e2.encrypt("test-secret");

    EXPECT_NE(ct1, ct2) << "Different keys should produce different ciphertext";
}

TEST_F(ApiKeyEncryptorTest, Base64OutputIsPrintable) {
    setenv("CHRONOS_MASTER_KEY", "test-key", 1);
    ApiKeyEncryptor encryptor;

    std::string encrypted = encryptor.encrypt("secret-data");
    // Base64 should only contain alphanumeric, +, /, =
    for (char c : encrypted) {
        EXPECT_TRUE(std::isalnum(c) || c == '+' || c == '/' || c == '=')
            << "Unexpected char in base64: " << c;
    }
}

// ============================================================================
// 2. LogSanitizer — sensitive data redaction
// ============================================================================

TEST(LogSanitizerTest, MasksApiKeyHexStrings) {
    LogSanitizer sanitizer;
    std::string input = "API_KEY=64a7c9e1f83b2d05a4e6c8f09d1b3e57abc123";
    std::string result = sanitizer.sanitize(input);

    EXPECT_NE(result, input);
    // The hex part should be masked
    EXPECT_NE(result.find("***"), std::string::npos);
    // Non-sensitive parts should be preserved
    EXPECT_NE(result.find("API_KEY="), std::string::npos);
}

TEST(LogSanitizerTest, PreservesShortHexStrings) {
    LogSanitizer sanitizer;
    std::string input = "symbol_id=1 order_id=5001";
    std::string result = sanitizer.sanitize(input);
    EXPECT_EQ(result, input) << "Short hex should not be masked";
}

TEST(LogSanitizerTest, MasksPrivateKeyPemBlocks) {
    LogSanitizer sanitizer;
    std::string input = R"(Config loaded
-----BEGIN PRIVATE KEY-----
MIIEvQIBADANBgkqhkiG9w0BAQEFAASC
-----END PRIVATE KEY-----
Done)";
    std::string result = sanitizer.sanitize(input);

    // Markers preserved, content masked
    EXPECT_NE(result.find("-----BEGIN PRIVATE KEY-----"), std::string::npos);
    EXPECT_NE(result.find("-----END PRIVATE KEY-----"), std::string::npos);
    EXPECT_NE(result.find("*"), std::string::npos);
}

TEST(LogSanitizerTest, MasksLongTokens) {
    LogSanitizer sanitizer;
    std::string input = "Authorization: eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0";
    std::string result = sanitizer.sanitize(input);

    EXPECT_NE(result, input);
    // Token should be partially masked (first 8 kept, last 4 kept)
    EXPECT_NE(result.find("eyJhbGci"), std::string::npos) << "First 8 chars preserved";
    EXPECT_NE(result.find("*"), std::string::npos) << "Content masked";
}

TEST(LogSanitizerTest, DetectsSensitiveData) {
    LogSanitizer sanitizer;
    EXPECT_TRUE(sanitizer.containsSensitiveData("API_KEY: 64a7c9e1f83b2d05a4e6c8f09d1b3e57"));
    EXPECT_TRUE(sanitizer.containsSensitiveData("-----BEGIN RSA PRIVATE KEY-----"));
    EXPECT_FALSE(sanitizer.containsSensitiveData("price=50000.0 quantity=0.1"));
}

TEST(LogSanitizerTest, DisabledMasksDoNothing) {
    LogSanitizer::Config cfg;
    cfg.mask_api_keys = false;
    cfg.mask_private_keys = false;
    cfg.mask_tokens = false;
    LogSanitizer sanitizer(cfg);

    std::string input = "API_KEY=64a7c9e1f83b2d05a4e6c8f09d1b3e57abc123";
    EXPECT_EQ(sanitizer.sanitize(input), input);
}

// ============================================================================
// 3. TokenValidator — authentication + rate limiting
// ============================================================================

TEST(TokenValidatorTest, ValidTokenAccepted) {
    TokenValidator::Config cfg;
    cfg.valid_token = "secret-token-abc";
    TokenValidator validator(cfg);

    EXPECT_TRUE(validator.validate("secret-token-abc"));
    EXPECT_EQ(validator.accepted_count(), 1u);
    EXPECT_EQ(validator.rejected_count(), 0u);
}

TEST(TokenValidatorTest, InvalidTokenRejected) {
    TokenValidator::Config cfg;
    cfg.valid_token = "correct-token";
    TokenValidator validator(cfg);

    EXPECT_FALSE(validator.validate("wrong-token"));
    EXPECT_EQ(validator.accepted_count(), 0u);
    EXPECT_EQ(validator.rejected_count(), 1u);
}

TEST(TokenValidatorTest, WrongLengthTokenRejected) {
    TokenValidator::Config cfg;
    cfg.valid_token = "exact-token";
    TokenValidator validator(cfg);

    EXPECT_FALSE(validator.validate("exact-token-extra"));
    EXPECT_FALSE(validator.validate("short"));
    EXPECT_EQ(validator.rejected_count(), 2u);
}

TEST(TokenValidatorTest, RateLimitEnforced) {
    TokenValidator::Config cfg;
    cfg.valid_token = "token";
    cfg.max_requests_per_second = 5;
    TokenValidator validator(cfg);

    // First 5 should pass (assuming all in same second)
    int accepted = 0;
    for (int i = 0; i < 10; ++i) {
        if (validator.validate("token")) accepted++;
    }

    EXPECT_LE(accepted, 5) << "Rate limit should cap at 5";
    EXPECT_GT(validator.rejected_count(), 0u)
        << "Some should be rate-limited";
}

TEST(TokenValidatorTest, RateLimitResets) {
    TokenValidator::Config cfg;
    cfg.valid_token = "token";
    cfg.max_requests_per_second = 100;
    TokenValidator validator(cfg);

    // Use all requests
    for (int i = 0; i < 100; ++i) {
        validator.validate("token");
    }
    // Rate limited now
    EXPECT_FALSE(validator.checkRateLimit());

    // Reset
    validator.resetRateLimit();
    EXPECT_TRUE(validator.checkRateLimit());
}

TEST(TokenValidatorTest, ConcurrentValidation) {
    TokenValidator::Config cfg;
    cfg.valid_token = "shared-token";
    cfg.max_requests_per_second = 1000000;
    TokenValidator validator(cfg);

    std::atomic<int> ok{0};
    std::atomic<int> fail{0};

    auto worker = [&]() {
        for (int i = 0; i < 1000; ++i) {
            if (validator.validate("shared-token")) {
                ok.fetch_add(1, std::memory_order_relaxed);
            } else {
                fail.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    std::thread t1(worker);
    std::thread t2(worker);
    std::thread t3(worker);
    std::thread t4(worker);

    t1.join(); t2.join(); t3.join(); t4.join();

    EXPECT_GT(ok.load(), 0);
    // With generous rate limit, all should pass
    EXPECT_EQ(fail.load(), 0);
}
