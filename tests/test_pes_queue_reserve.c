#include <stdio.h>
#include "../source/audio/pes_queue_reserve.h"

static int checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); } } while (0)

int main(void) {
    unsigned char flags[8] = {0};
    CHECK(pes_queue_discard_count(0, 8, 0, 0, 1) == -1);
    CHECK(pes_queue_discard_count(flags, 8, 0, 0, 9) == -1);
    CHECK(pes_queue_discard_count(flags, 8, 0, 9, 1) == -1);
    CHECK(pes_queue_discard_count(flags, 8, 8, 0, 1) == -1);
    CHECK(pes_queue_discard_count(flags, 8, 0, 0, 0) == -1);
    /* Full queue, split PES wraps across end of the ring. */
    flags[7] = flags[0] = 1;
    CHECK(pes_queue_discard_count(flags, 8, 6, 8, 1) == 3);
    CHECK(pes_queue_discard_count(flags, 8, 7, 7, 2) == 2);
    CHECK(pes_queue_discard_count(flags, 8, 6, 8, 8) == 8);
    /* All queue layouts and request sizes, including a partially consumed
     * oldest PES. No eviction when room exists; otherwise the minimal
     * sufficient eviction must finish exactly at a group boundary. */
    for (unsigned mask = 0; mask < 256; ++mask) {
        for (int i = 0; i < 8; ++i) flags[i] = (mask >> i) & 1;
        for (int read = 0; read < 8; ++read) {
            for (int used = 0; used <= 8; ++used) {
                for (int required = 1; required <= 8; ++required) {
                    int expected = required - (8 - used);
                    if (expected < 0) expected = 0;
                    if (expected) {
                        while (expected < used && flags[(read + expected) % 8])
                            ++expected;
                    }
                    int actual = pes_queue_discard_count(flags, 8, read, used, required);
                    CHECK(actual == expected);
                    CHECK(actual >= 0 && actual <= used);
                    CHECK(8 - used + actual >= required);
                    CHECK(!actual || actual == used || !flags[(read + actual) % 8]);
                }
            }
        }
    }
    printf("PES queue reservation: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
