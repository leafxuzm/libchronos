#include "chronos/io/security/security.hpp"
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <regex>
#include <chrono>

namespace chronos {
namespace security {

namespace {

// --- Base64 helpers (no external dependency) ---

const char BASE64_CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const std::vector<uint8_t>& data) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    for (size_t i = 0; i < data.size(); i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < data.size()) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < data.size()) n |= static_cast<uint32_t>(data[i + 2]);
        out.push_back(BASE64_CHARS[(n >> 18) & 0x3F]);
        out.push_back(BASE64_CHARS[(n >> 12) & 0x3F]);
        out.push_back((i + 1 < data.size()) ? BASE64_CHARS[(n >> 6) & 0x3F] : '=');
        out.push_back((i + 2 < data.size()) ? BASE64_CHARS[n & 0x3F] : '=');
    }
    return out;
}

std::vector<uint8_t> base64Decode(const std::string& encoded) {
    std::vector<uint8_t> out;
    out.reserve(encoded.size() * 3 / 4);
    int val = 0, valb = -8;
    for (char c : encoded) {
        if (c == '=') break;
        const char* p = std::strchr(BASE64_CHARS, c);
        if (!p) continue;
        val = (val << 6) | static_cast<int>(p - BASE64_CHARS);
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

// --- XOR cipher ---

std::vector<uint8_t> xorCipher(const std::vector<uint8_t>& data,
                                const std::vector<uint8_t>& key) {
    if (key.empty()) return data;
    std::vector<uint8_t> out(data.size());
    for (size_t i = 0; i < data.size(); ++i) {
        out[i] = data[i] ^ key[i % key.size()];
    }
    return out;
}

// --- Key derivation from env ---

std::vector<uint8_t> deriveKey(const std::string& master) {
    if (master.empty()) return {};
    // Simple derivation: repeat and hash-like expansion
    std::vector<uint8_t> key(32);
    for (size_t i = 0; i < 32; ++i) {
        key[i] = static_cast<uint8_t>(
            master[i % master.size()] ^ ((i * 0x9E3779B9u) & 0xFF));
    }
    return key;
}

} // anonymous namespace

// ============================================================================
// ApiKeyEncryptor
// ============================================================================

ApiKeyEncryptor::ApiKeyEncryptor() {
    const char* env = std::getenv("CHRONOS_MASTER_KEY");
    if (env && env[0] != '\0') {
        master_key_ = deriveKey(std::string(env));
    }
}

std::string ApiKeyEncryptor::encrypt(const std::string& plaintext) const {
    if (master_key_.empty()) return plaintext; // no key configured, store as-is

    std::vector<uint8_t> data(plaintext.begin(), plaintext.end());
    auto encrypted = xorCipher(data, master_key_);
    return base64Encode(encrypted);
}

std::string ApiKeyEncryptor::decrypt(const std::string& ciphertext_b64) const {
    if (master_key_.empty()) return ciphertext_b64; // not encrypted

    auto data = base64Decode(ciphertext_b64);
    auto decrypted = xorCipher(data, master_key_);
    return std::string(decrypted.begin(), decrypted.end());
}

// ============================================================================
// LogSanitizer
// ============================================================================

std::string LogSanitizer::sanitize(const std::string& input) const {
    std::string result = input;
    if (config_.mask_api_keys)     result = maskApiKeys(result, config_.mask_char);
    if (config_.mask_private_keys) result = maskPrivateKeys(result, config_.mask_char);
    if (config_.mask_tokens)       result = maskTokens(result, config_.mask_char);
    return result;
}

bool LogSanitizer::containsSensitiveData(const std::string& input) const {
    // Check for API key-like patterns (long hex strings)
    if (config_.mask_api_keys) {
        std::regex hex_re(R"(\b[0-9a-fA-F]{32,}\b)");
        if (std::regex_search(input, hex_re)) return true;
    }
    // Check for private key markers
    if (config_.mask_private_keys) {
        if (input.find("BEGIN RSA PRIVATE KEY") != std::string::npos ||
            input.find("BEGIN EC PRIVATE KEY") != std::string::npos ||
            input.find("BEGIN PRIVATE KEY") != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string LogSanitizer::maskApiKeys(const std::string& input, char mask) {
    // Replace hex strings >= 32 chars (typical API key length)
    std::regex hex_re(R"(\b[0-9a-fA-F]{32,}\b)");
    std::string result;
    auto it = std::sregex_iterator(input.begin(), input.end(), hex_re);
    auto end = std::sregex_iterator();
    size_t last = 0;
    for (auto i = it; i != end; ++i) {
        result.append(input, last, i->position() - last);
        result.append(i->length(), mask);
        last = i->position() + i->length();
    }
    result.append(input, last, input.length() - last);
    return result;
}

std::string LogSanitizer::maskPrivateKeys(const std::string& input, char mask) {
    std::string result = input;

    // Mask content between PEM markers
    const char* begin_markers[] = {
        "-----BEGIN RSA PRIVATE KEY-----",
        "-----BEGIN EC PRIVATE KEY-----",
        "-----BEGIN PRIVATE KEY-----",
    };
    const char* end_markers[] = {
        "-----END RSA PRIVATE KEY-----",
        "-----END EC PRIVATE KEY-----",
        "-----END PRIVATE KEY-----",
    };

    for (size_t i = 0; i < 3; ++i) {
        size_t pos = 0;
        while ((pos = result.find(begin_markers[i], pos)) != std::string::npos) {
            size_t content_start = pos + std::strlen(begin_markers[i]);
            size_t end_pos = result.find(end_markers[i], content_start);
            if (end_pos == std::string::npos) break;

            // Mask content between markers (preserve markers + newline)
            for (size_t j = content_start; j < end_pos; ++j) {
                if (result[j] != '\n' && result[j] != '\r') {
                    result[j] = mask;
                }
            }
            pos = end_pos + std::strlen(end_markers[i]);
        }
    }
    return result;
}

std::string LogSanitizer::maskTokens(const std::string& input, char mask) {
    // Mask JWT-like tokens and long base64 strings (> 40 chars)
    // Matches: base64url chars + dots/dashes (JWT separators)
    std::regex token_re(R"([A-Za-z0-9+/=._-]{40,})");
    std::string result;
    auto it = std::sregex_iterator(input.begin(), input.end(), token_re);
    auto end = std::sregex_iterator();
    size_t last = 0;
    for (auto i = it; i != end; ++i) {
        result.append(input, last, i->position() - last);
        // Keep first 8 and last 4 chars for identification
        std::string token = i->str();
        if (token.length() > 16) {
            result.append(token, 0, 8);
            result.append(token.length() - 16, mask);
            result.append(token, token.length() - 4, 4);
        } else {
            result.append(token.length(), mask);
        }
        last = i->position() + i->length();
    }
    result.append(input, last, input.length() - last);
    return result;
}

// ============================================================================
// TokenValidator
// ============================================================================

namespace {

uint32_t currentSecond() {
    auto now = std::chrono::system_clock::now();
    auto dur = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch());
    return static_cast<uint32_t>(dur.count());
}

// Constant-time comparison to prevent timing attacks
bool constantTimeEquals(const std::string& a, const std::string& b) {
    if (a.length() != b.length()) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < a.length(); ++i) {
        diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    }
    return diff == 0;
}

} // anonymous namespace

bool TokenValidator::validate(const std::string& token) {
    if (!checkRateLimit()) {
        rejected_++;
        return false;
    }

    if (!constantTimeEquals(token, config_.valid_token)) {
        rejected_++;
        return false;
    }

    accepted_++;
    return true;
}

bool TokenValidator::checkRateLimit() {
    uint32_t now_sec = currentSecond();
    if (current_second_ != now_sec) {
        current_second_ = now_sec;
        requests_this_second_ = 0;
    }
    requests_this_second_++;
    return requests_this_second_ <= config_.max_requests_per_second;
}

void TokenValidator::resetRateLimit() {
    current_second_ = 0;
    requests_this_second_ = 0;
}

}  // namespace security
}  // namespace chronos
