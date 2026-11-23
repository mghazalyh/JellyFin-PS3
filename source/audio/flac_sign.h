#ifndef JF_FLAC_SIGN_H
#define JF_FLAC_SIGN_H
#include <stdint.h>

// FLAC residuals are 1..32 bits. Avoid negative signed shifts and
// implementation-dependent unsigned-to-signed integer conversions.
static inline int32_t flac_sign_extend(uint32_t value, unsigned bits) {
    if (bits == 0 || bits > 32) return 0;
    const uint64_t sign = UINT64_C(1) << (bits - 1);
    const uint64_t mask = (sign << 1) - 1;
    const uint64_t payload = (uint64_t)value & mask;
    const int64_t result = (int64_t)payload -
        ((payload & sign) ? (int64_t)(sign << 1) : 0);
    return (int32_t)result;
}
#endif
