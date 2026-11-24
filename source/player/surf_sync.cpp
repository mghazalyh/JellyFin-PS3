// Correlated Surf source transitions: source status -> decoded picture
// discontinuity -> completed RSX flip -> PCM DMA gate release.
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "surf_sync.h"
#include "surf_gate_logic.h"
#include "audio.h"
#include "audio_bitstream.h"
#include "timing.h"
#include "plog.h"

// Tiny fixed shared state. Each critical section is a copy or 64-byte sample
// comparison; never perform network or GPU work under this spin lock.
static volatile int s_lock = 0;
static void lock_state(void) {
    while (__sync_lock_test_and_set(&s_lock, 1)) { }
    __sync_synchronize();
}
static void unlock_state(void) {
    __sync_synchronize();
    __sync_lock_release(&s_lock);
}
static u32 fnv1a(const char *s) {
    u32 hash = 2166136261u;
    if (!s) return hash;
    for (const unsigned char *p=(const unsigned char*)s; *p; p++)
        hash = (hash ^ *p) * 16777619u;
    return hash;
}

static bool s_active = false, s_have_source = false;
static bool s_enabled = false; // per session, opt-in by controller
static u32 s_generation = 0, s_epoch = 0;
static u32 s_session_nonce = 0, s_connection = 0;
static u32 s_source_hash = 0, s_boot_hash = 0;
static char s_mode[24], s_source[152];
static bool s_transition_pending = false, s_gating = false;
static bool s_have_luma = false, s_cut_found = false;
static u8 s_luma[64] = {0};
static u32 s_last_luma_seq = 0, s_candidate_seq = 0;
static u32 s_flip_seq = 0;
static bool s_flip_qualified = false;
static u64 s_candidate_pts = 0;
static u64 s_gate_at = 0;
static unsigned s_cut_avg = 0, s_cut_changed = 0;


static SurfSyncEvent s_events[16];
static unsigned s_ev_read = 0, s_ev_write = 0;

static void event_locked(const char *event, const char *state) {
    const unsigned next = (s_ev_write + 1) % 16;
    if (next == s_ev_read) {
        s_ev_read = (s_ev_read + 1) % 16;  // bounded; retain newest
    }
    SurfSyncEvent *e = &s_events[s_ev_write];
    memset(e, 0, sizeof *e);
    snprintf(e->event, sizeof e->event, "%s", event);
    snprintf(e->mode, sizeof e->mode, "%s", s_mode);
    snprintf(e->source, sizeof e->source, "%s", s_source);
    snprintf(e->trace, sizeof e->trace, "ps3p%08x%08x_%04x_%04x",
             (unsigned)s_session_nonce, (unsigned)s_generation,
             (unsigned)(s_connection & 0xffffu), (unsigned)(s_epoch & 0xffffu));
    snprintf(e->state, sizeof e->state, "%s", state);
    s_ev_write = next;
}

bool surf_sync_take_event(SurfSyncEvent *out) {
    if (!out) return false;
    lock_state();
    if (s_ev_read == s_ev_write) { unlock_state(); return false; }
    *out = s_events[s_ev_read];
    s_ev_read = (s_ev_read + 1) % 16;
    unlock_state();
    return true;
}

void surf_sync_health_event(const char *event, const char *reason) {
    if (!event || !reason) return;
    lock_state();
    if (s_active) event_locked(event, reason);
    unlock_state();
}

void surf_sync_trace_prefix(char *out, size_t cap) {
    if (!out || !cap) return;
    lock_state();
    snprintf(out, cap, "ps3p%08x%08x", (unsigned)s_session_nonce, (unsigned)s_generation);
    unlock_state();
}

static void sync_session(const char *mode, u32 generation, bool reconnect) {
    // The old audio ring must never stay muted in a new playback.
    lock_state();
    audio_set_transition_gate(false);
    const bool same = reconnect && s_active && mode && !strcmp(mode, s_mode) &&
                      generation == s_generation;
    if (same) {
        ++s_connection;
    } else {
        s_session_nonce = (u32)timing_get_us();
        s_connection = 0;
    }
    s_active = mode && mode[0];
    s_enabled = same && s_enabled;
    snprintf(s_mode, sizeof s_mode, "%s", mode ? mode : "");
    s_source[0] = '\0';
    s_generation = generation;
    s_epoch = 0; s_source_hash = s_boot_hash = 0;
    s_have_source = false;
    s_transition_pending = s_gating = false;
    s_have_luma = s_cut_found = false;
    s_last_luma_seq = s_candidate_seq = s_flip_seq = 0;
    s_flip_qualified = false;
    s_candidate_pts = s_gate_at = 0;
    if (!same) s_ev_read = s_ev_write = 0;
    if (same) event_locked("playback_started", "stream_reopened");
    unlock_state();
}

void surf_sync_session(const char *mode, u32 generation) {
    sync_session(mode, generation, false);
}
void surf_sync_reconnect(const char *mode, u32 generation) {
    sync_session(mode, generation, true);
}

void surf_sync_set_enabled(bool enabled) {
    bool changed = false, next = false;
    lock_state();
    next = s_active && enabled;
    if (s_enabled != next) {
        s_enabled = next;
        s_transition_pending = false;
        s_gating = false;
        s_cut_found = false;
        s_flip_qualified = false;
        audio_set_transition_gate(false);
        changed = true;
        event_locked("source_changed", next ? "sync_beta_enabled" : "sync_beta_disabled");
    }
    unlock_state();
    if (changed) {
        plog(next ? "surf_sync: experimental PCM sync ENABLED" :
                    "surf_sync: experimental PCM sync DISABLED");
    }
}
bool surf_sync_enabled(void) {
    lock_state();
    const bool enabled = s_active && s_enabled;
    unlock_state();
    return enabled;
}

void surf_sync_stop(void) {
    lock_state();
    s_active = false; s_transition_pending = false;
    s_gating = false; s_have_source = false;
    s_flip_qualified = false;
    audio_set_transition_gate(false);
    s_ev_read = s_ev_write = 0;
    unlock_state();
}

void surf_sync_observe_source(const char *mode, const char *source,
                              const char *source_id, const char *boot,
                              u32 generation) {
    if (!mode || !source || !source[0] || !source_id || !source_id[0])
        return;
    const u32 sh = fnv1a(source_id), bh = fnv1a(boot);
    lock_state();
    if (!s_active || generation != s_generation || strcmp(mode, s_mode)) {
        unlock_state(); return;
    }
    if (!s_have_source) {
        s_have_source = true;
        s_source_hash = sh; s_boot_hash = bh;
        snprintf(s_source, sizeof s_source, "%s", source);
        event_locked("source_changed", "initial_source");
        unlock_state(); return;
    }
    bool changed = false;
    u32 epoch = 0;
    if (sh != s_source_hash || bh != s_boot_hash) {
        s_source_hash = sh; s_boot_hash = bh;
        snprintf(s_source, sizeof s_source, "%s", source);
        s_epoch++;
        epoch = s_epoch;
        s_transition_pending = s_enabled;
        // Superseding a transition invalidates every unconfirmed flip and cut.
        s_flip_qualified = false;
        s_cut_found = false;
        event_locked("source_changed", s_enabled ? "transition_observed" :
                                                  "observed_sync_disabled");
        changed = true;
    }
    unlock_state();
    if (changed) {
        char b[116];
        snprintf(b, sizeof b, "surf_sync: epoch=%u source_hash=%08x observed",
                 (unsigned)epoch, (unsigned)sh);
        plog(b);
    }
}

void surf_sync_display_tick(bool paused) {
    bool start = false, release = false;
    u64 age_ms = 0;
    u32 epoch = 0;
    const u64 now = timing_get_us();
    lock_state();
    if (s_active && s_enabled && s_transition_pending && !paused) {
        s_transition_pending = false;
        const bool already_gated = s_gating;
        s_gating = !audio_passthrough_active();
        // Repeated rotations cannot indefinitely extend a continuous mute.
        if (!already_gated) s_gate_at = now;
        s_cut_found = false;
        s_flip_qualified = false;
        s_have_luma = false;
        // Atomic flag only; serialize it with the state transition.
        audio_set_transition_gate(s_gating);
        start = s_gating;
        epoch = s_epoch;
        event_locked("source_changed", start ? "audio_gate_started" :
                                                    "bitstream_gate_skipped");
    }
    // Recovery ceiling, not the synchronization strategy: avoid a permanently
    // silent stream when a channel freezes or no visible scene cut is detected.
    if (s_active && s_enabled && s_gating && !paused &&
        now >= s_gate_at && now - s_gate_at >= 3000000ULL) {
        s_gating = false;
        s_flip_qualified = false;
        audio_set_transition_gate(false);
        release = true;
        age_ms = (now - s_gate_at) / 1000ULL;
        epoch = s_epoch;
        event_locked("playback_stalled", "gate_timeout_recovery");
    }
    unlock_state();
    if (start) {
        char b[100];
        snprintf(b, sizeof b, "surf_sync: epoch=%u pcm_gate=ON", (unsigned)epoch);
        plog(b);
    }
    if (release) {
        char b[110];
        snprintf(b, sizeof b, "surf_sync: epoch=%u timeout=%llums pcm_gate=OFF",
                 (unsigned)epoch, (unsigned long long)age_ms);
        plog(b);
    }
}

void surf_sync_probe_video(const u8 *y, u32 width, u32 height, u32 sequence,
                            u64 pts_us) {
    if (!y || width < 16 || height < 16) return;
    u8 now[64];
    for (u32 row = 0; row < 8; row++) {
        const size_t off = (size_t)((row * 2 + 1) * height / 16) * width;
        for (u32 col = 0; col < 8; col++) {
            const u32 x = (col * 2 + 1) * width / 16;
            now[row * 8 + col] = y[off + x];
        }
    }
    lock_state();
    if (s_active && s_enabled && !s_transition_pending &&
        (!s_have_luma || sequence != s_last_luma_seq)) {
        if (s_gating && s_have_luma && !s_cut_found) {
            unsigned avg = 0, changed = 0;
            if (surf_cut_detect(s_luma, now, 64, &avg, &changed)) {
                s_cut_found = true;
                s_cut_avg = avg; s_cut_changed = changed;
                s_candidate_seq = sequence;
                s_candidate_pts = pts_us;
                event_locked("source_changed", "visual_cut_decoded");
            }
        }
        memcpy(s_luma, now, sizeof now);
        s_have_luma = true;
        s_last_luma_seq = sequence;
    }
    unlock_state();
}

void surf_sync_flip_queued(u32 sequence, u64 pts_us) {
    lock_state();
    s_flip_seq = sequence;
    // Qualify the submitted picture now, not retroactively on completion.
    s_flip_qualified = s_active && s_enabled && s_gating &&
                       !s_transition_pending && s_cut_found &&
                       surf_seq_reached(sequence, s_candidate_seq);
    (void)pts_us;
    unlock_state();
}

void surf_sync_flip_completed(bool success) {
    bool release = false;
    u32 epoch = 0, seq = 0, avg = 0, changed = 0;
    u64 pts = 0, duration = 0;
    lock_state();
    const bool qualified = s_flip_qualified;
    s_flip_qualified = false;  // Consume once, even if the flip failed.
    if (success && qualified && s_active && s_enabled && s_gating &&
        !s_transition_pending && s_cut_found &&
        surf_seq_reached(s_flip_seq, s_candidate_seq)) {
        s_gating = false;
        audio_set_transition_gate(false);
        release = true; epoch = s_epoch;
        seq = s_flip_seq; pts = s_candidate_pts;
        avg = s_cut_avg; changed = s_cut_changed;
        const u64 now = timing_get_us();
        duration = now >= s_gate_at ? now - s_gate_at : 0;
        event_locked("source_changed", "video_flip_audio_release");
    }
    unlock_state();
    if (release) {
        char b[128];
        snprintf(b, sizeof b,
                 "surf_sync: epoch=%u flip_done seq=%u pts=%lluus gate=%llums cut=%u/%u",
                 (unsigned)epoch, (unsigned)seq,
                 (unsigned long long)pts,
                 (unsigned long long)(duration / 1000ULL),avg,changed);
        plog(b);
    }
}
