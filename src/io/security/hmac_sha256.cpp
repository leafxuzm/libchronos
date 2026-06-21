#include <chronos/io/security/hmac_sha256.hpp>

#include <openssl/hmac.h>
#include <sstream>
#include <iomanip>

namespace chronos {
namespace security {

std::string hmacSha256(const std::string& key, const std::string& data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;

    HMAC(EVP_sha256(),
         key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(),
         digest, &digest_len);

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < digest_len; ++i)
        oss << std::setw(2) << static_cast<int>(digest[i]);
    return oss.str();
}

}  // namespace security
}  // namespace chronos
