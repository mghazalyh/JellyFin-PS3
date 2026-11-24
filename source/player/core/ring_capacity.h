#pragma once
#include <stdint.h>

#define RING_BYTES_MIN (2u * 1024u * 1024u)
#define RING_BYTES_MAX (40u * 1024u * 1024u)

// All allocation bounds and fallback decisions use whole packets.
// The nominal byte floor rounds DOWN by at most one packet to preserve
// the allocation budget, and is always attempted once before giving up.
static inline unsigned ring_initial_packets(uint32_t want, unsigned packet_size) {
    if (!packet_size || packet_size > RING_BYTES_MIN) return 0;
    if (want < RING_BYTES_MIN) want = RING_BYTES_MIN;
    if (want > RING_BYTES_MAX) want = RING_BYTES_MAX;
    return want / packet_size;
}

static inline unsigned ring_next_packets(unsigned current, unsigned packet_size) {
    if (!packet_size || packet_size > RING_BYTES_MIN) return 0;
    const unsigned floor = RING_BYTES_MIN / packet_size;
    if (current <= floor) return 0;
    const unsigned half = current / 2u;
    return half < floor ? floor : half;
}
