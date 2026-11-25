#pragma once
#include <stdint.h>
#include "../util/live_pts_delta.h"

// Main display thread only. Never changes audio PTS, PCM, or buffer sizes.
// The upper bound stays below the live presentation watchdog's 2.5 s window.
// A frozen/invalid audio clock must not pin the video upload queue.
enum SurfDisplayDecision {
    SURF_DISPLAY_PASS = 0,
    SURF_DISPLAY_HOLD_BEGIN,
    SURF_DISPLAY_HOLD,
    SURF_DISPLAY_RESUME,
    SURF_DISPLAY_ABORT
};
struct SurfDisplayGuard {
    uint64_t started_us, progress_us, last_audio_us;
    bool holding, suppressed;
};
static inline void surf_display_guard_reset(SurfDisplayGuard *g) {
    *g = SurfDisplayGuard{};
}
static inline SurfDisplayDecision surf_display_guard_step(
    SurfDisplayGuard *g, bool enabled, uint64_t now, uint64_t video, uint64_t audio) {
    if (!enabled || !video || !audio) {
        surf_display_guard_reset(g);
        return SURF_DISPLAY_PASS;
    }
    const int64_t ahead = live_pts_delta_us(video, audio);
    if (g->holding) {
        if (now < g->started_us || now < g->progress_us ||
            live_pts_delta_us(audio, g->last_audio_us) < 0) {
            surf_display_guard_reset(g);
            g->suppressed = true;
            return SURF_DISPLAY_ABORT;
        }
        if (ahead <= 60000) {
            surf_display_guard_reset(g);
            return SURF_DISPLAY_RESUME;
        }
        if (live_pts_delta_us(audio, g->last_audio_us) > 0) {
            g->last_audio_us = audio;
            g->progress_us = now;
        }
        if (now - g->started_us >= 2400000ULL ||
            now - g->progress_us >= 400000ULL) {
            g->holding = false;
            g->suppressed = true;
            return SURF_DISPLAY_ABORT;
        }
        return SURF_DISPLAY_HOLD;
    }
    // Do not repeatedly freeze on the same unresolved discontinuity.
    // Rearm only after a normal offset or a new playback/clock context.
    if (g->suppressed) {
        if (ahead >= -60000 && ahead <= 60000) g->suppressed = false;
        return SURF_DISPLAY_PASS;
    }
    if (ahead <= 150000 || ahead > 5000000) return SURF_DISPLAY_PASS;
    g->started_us = g->progress_us = now;
    g->last_audio_us = audio;
    g->holding = true;
    return SURF_DISPLAY_HOLD_BEGIN;
}
