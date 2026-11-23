#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../source/audio/flac_sign.h"
int main(void) {
    assert(flac_sign_extend(1u, 1) == -1);
    assert(flac_sign_extend(0u, 1) == 0);
    assert(flac_sign_extend(0x7FFFFFFFu, 32) == INT32_MAX);
    assert(flac_sign_extend(0x80000000u, 32) == INT32_MIN);
    assert(flac_sign_extend(0xFFFFFFFFu, 32) == -1);
    assert(flac_sign_extend(0u, 0) == 0);
    assert(flac_sign_extend(0u, 33) == 0);
    for (unsigned bits = 1; bits <= 32; ++bits) {
        const uint64_t sign = UINT64_C(1) << (bits - 1);
        const uint64_t mask = (sign << 1) - 1;
        assert(flac_sign_extend((uint32_t)(sign - 1),bits) == (int32_t)(sign - 1));
        assert(flac_sign_extend((uint32_t)sign,bits) == -(int64_t)sign);
        assert(flac_sign_extend((uint32_t)mask,bits) == -1);
        for (unsigned j = 0; j < 1024; ++j) {
            const uint32_t value = j * 2654435761u + bits * 33u;
            const uint64_t payload = (uint64_t)value & mask;
            const int64_t expected = (int64_t)payload -
                ((payload & sign) ? (int64_t)(sign << 1) : 0);
            assert(flac_sign_extend(value,bits) == expected);
        }
    }
    puts("FLAC signed residual sign-extension test passed");
    return 0;
}
