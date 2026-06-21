#pragma once

#include <string>

namespace chronos {
namespace security {

/// HMAC-SHA256 — used for Binance REST API request signing.
/// @param key   Secret key (API secret)
/// @param data  Data to sign (query string)
/// @return      Hex-encoded HMAC-SHA256 digest (64 hex chars)
std::string hmacSha256(const std::string& key, const std::string& data);

}  // namespace security
}  // namespace chronos
