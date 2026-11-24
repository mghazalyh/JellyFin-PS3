#pragma once
#include <stdint.h>

// The PCM ring always holds 48 kHz sample frames. One frame is exactly
// 125/6 microseconds. Carry the fractional remainder across blocks instead
// of truncating every read/write; at 256 frames the old clock lost 225 ms/h.
typedef struct {
    uint64_t read_us, write_us;
    unsigned read_remainder, write_remainder;
    int valid;
} audio_pts_state;

static inline void audio_pts_reset(audio_pts_state *s) {
    s->read_us = s->write_us = 0;
    s->read_remainder = s->write_remainder = 0;
    s->valid = 0;
}

static inline uint64_t audio_pts_delta_us(unsigned frames, unsigned *remainder) {
    const uint64_t scaled = (uint64_t)frames * 125u + *remainder;
    *remainder = (unsigned)(scaled % 6u);
    return scaled / 6u;
}

// Caller holds the PCM mutex. A seek clears the PCM state under that same
// mutex after changing the flush generation. An older packet must never
// seed the clock after that reset, even when its PCM would be rejected.
static inline int audio_pts_seed(audio_pts_state *s, uint64_t pts_us,
                                 uint32_t packet_gen, uint32_t flush_gen) {
    if (packet_gen != flush_gen) return 0;
    s->write_us = pts_us;
    s->write_remainder = 0;
    if (!s->valid) {
        s->read_us = pts_us;
        s->read_remainder = 0;
        s->valid = 1;
    }
    return 1;
}

static inline void audio_pts_advance_read(audio_pts_state *s, unsigned frames) {
    s->read_us += audio_pts_delta_us(frames, &s->read_remainder);
}

static inline void audio_pts_advance_write(audio_pts_state *s, unsigned frames) {
    s->write_us += audio_pts_delta_us(frames, &s->write_remainder);
}
