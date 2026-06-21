#include <chronos/core/types.hpp>
#include <cstdlib>

namespace chronos {

Decimal parseDecimal(std::string_view sv) {
    if (sv.empty()) return Decimal{0};

    const char* p = sv.data();
    const char* end = p + sv.size();

    // Skip leading whitespace
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    if (p >= end) return Decimal{0};

    // Sign
    bool negative = false;
    if (*p == '-') { negative = true; ++p; }
    else if (*p == '+') { ++p; }

    // Parse integer part
    int64_t mantissa = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        mantissa = mantissa * 10 + (*p - '0');
        ++p;
    }

    // Parse fractional part (up to 10 decimal digits for precision)
    int frac_digits = 0;
    int64_t frac_part = 0;
    if (p < end && *p == '.') {
        ++p;
        while (p < end && *p >= '0' && *p <= '9' && frac_digits < 10) {
            frac_part = frac_part * 10 + (*p - '0');
            ++frac_digits;
            ++p;
        }
        // Skip remaining fractional digits beyond precision
        while (p < end && *p >= '0' && *p <= '9') ++p;
    }

    // Compute: raw = mantissa * 2^32 + frac_part * 2^32 / 10^frac_digits
    // Use __int128 to avoid overflow
    __int128 scaled_mantissa = static_cast<__int128>(mantissa) * (1ULL << 32);

    // Compute 10^frac_digits
    int64_t pow10 = 1;
    for (int i = 0; i < frac_digits; ++i) pow10 *= 10;

    __int128 scaled_frac = static_cast<__int128>(frac_part) * (1ULL << 32) / pow10;
    __int128 raw = scaled_mantissa + scaled_frac;

    if (negative) raw = -raw;

    return Decimal::from_raw_value(static_cast<int64_t>(raw));
}

} // namespace chronos
