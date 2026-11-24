#include <stdio.h>
#include <stdint.h>
#include "ring_capacity.h"

static int failures, checks;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main(void) {
    const unsigned ts = 188u;
    const unsigned floor = RING_BYTES_MIN / ts;
    CHECK(floor == 11155u);
    CHECK(ring_initial_packets(0u, ts) == floor);
    CHECK(ring_initial_packets(RING_BYTES_MIN, ts) == floor);
    CHECK(ring_initial_packets(UINT32_MAX, ts) == RING_BYTES_MAX / ts);
    CHECK(ring_next_packets(floor, ts) == 0);
    CHECK(ring_next_packets(floor + 1u, ts) == floor);
    CHECK(ring_initial_packets(100u, 0) == 0);
    CHECK(ring_next_packets(100u, 0) == 0);
    // Exhaust each input in steps, modeling allocation failure until the floor.
    for (uint32_t want = 0; want <= RING_BYTES_MAX + 1024u; want += 4093u) {
        unsigned cap = ring_initial_packets(want, ts);
        unsigned attempts = 0, floor_attempts = 0;
        while (cap) {
            CHECK(cap >= floor && cap <= RING_BYTES_MAX / ts);
            if (cap == floor) ++floor_attempts;
            unsigned next = ring_next_packets(cap, ts);
            CHECK(next < cap);
            cap = next;
            if (++attempts > 16) { CHECK(0); break; }
        }
        CHECK(floor_attempts == 1);
    }
    printf("ring capacity: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
