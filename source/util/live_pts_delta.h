#pragma once
#include <stdint.h>

// MPEG-TS timestamps are 33-bit at 90 kHz. The native decoder and PCM
// clock expose microseconds; rounding the wrap period to the nearest us
// introduces at most one microsecond error at a boundary. Live streams
// use a nearest modular difference, never an absolute seek-label offset.
#define LIVE_PTS_WRAP_US UINT64_C(95443717689)
static inline int64_t live_pts_delta_us(uint64_t video_us, uint64_t audio_us) {
    const int64_t period = (int64_t)LIVE_PTS_WRAP_US;
    int64_t diff = (int64_t)(video_us % LIVE_PTS_WRAP_US) -
                   (int64_t)(audio_us % LIVE_PTS_WRAP_US);
    if (diff > period / 2) diff -= period;
    if (diff < -period / 2) diff += period;
    return diff;
}
