// Opt-in PS3 FTP mailbox and compact local diagnostics.
// No filesystem I/O is performed by display, decoder, or audio threads.
#include "remote_mailbox.h"
#include "timing.h"
#include "plog.h"
#include "surf_sync.h"
#include "player_stats.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define JF_REMOTE_DIR "/dev_hdd0/tmp/jf_remote"
#define JF_REMOTE_CMD JF_REMOTE_DIR "/command.ready"
#define JF_REMOTE_KEY JF_REMOTE_DIR "/auth.key"
#define JF_REMOTE_RESPONSE JF_REMOTE_DIR "/response.txt"
#define JF_REMOTE_RESPONSE_TMP JF_REMOTE_DIR "/response.ps3.tmp"
#define JF_METRICS_PATH "/dev_hdd0/tmp/jf_metrics.bin"
#define JF_METRICS_PREV "/dev_hdd0/tmp/jf_metrics.prev.bin"
#define JF_METRICS_MAX (1024UL * 1024UL)

static volatile int s_lock = 0;
// 0: idle, 1: pending for display, 2: executing, 3: completion for writer,
// 4: completion being written by the progress worker.
static volatile int s_state = 0;
static JfRemoteCommand s_command;
static char s_detail[128];
static bool s_success = false;
static u32 s_result_generation = 0;
static volatile int s_io_owner = 0;
struct RemoteIoGuard {
    bool acquired;
    RemoteIoGuard() : acquired(__sync_bool_compare_and_swap(&s_io_owner, 0, 1)) {}
    ~RemoteIoGuard() { if (acquired) __sync_lock_release(&s_io_owner); }
};
static void sample_owned(u32 generation, u32 free_kb, u32 queue_depth,
                         u32 audio_clock_ms, u32 flags);
static bool s_sample_initialized = false;
static volatile u32 s_metric_drops = 0;
static JfRemoteReplay s_recent_ids = {};
static u64 s_command_started_us = 0;

static void lock_state(void) {
    while (!__sync_bool_compare_and_swap(&s_lock, 0, 1))
        usleep(100);
}
static void unlock_state(void) { __sync_lock_release(&s_lock); }

static void finish_locked(bool success, const char *detail, u32 generation) {
    s_success = success;
    snprintf(s_detail, sizeof s_detail, "%s", detail ? detail : "unspecified");
    s_result_generation = generation;
    s_state = 3;
}

void jf_remote_finish(const char *id, bool success, const char *detail, u32 generation) {
    if (!id) return;
    lock_state();
    if (s_state == 2 && generation == s_command.generation && !strcmp(id, s_command.id)) {
        finish_locked(success, detail, generation);
    }
    unlock_state();
}

bool jf_remote_take(JfRemoteCommand *out) {
    if (!out) return false;
    lock_state();
    const bool available = s_state == 1;
    if (available) {
        *out = s_command;
        // Do not retain the bearer secret any longer than necessary.
        memset(s_command.auth, 0, sizeof s_command.auth);
        s_state = 2;
        s_command_started_us = timing_get_us();
    }
    unlock_state();
    return available;
}

static bool write_result(const char *id, bool success, const char *detail,
                         u32 generation, const char *mode, u32 free_kb, u32 queue_depth) {
    FILE *f = fopen(JF_REMOTE_RESPONSE_TMP, "wb");
    if (!f) return false;
    char clean[128];
    size_t p = 0;
    if (detail) {
        for (const unsigned char *s = (const unsigned char *)detail;
             *s && p < sizeof clean - 1; ++s) {
            if (*s >= 32 && *s <= 126 && *s != '=') clean[p++] = (char)*s;
        }
    }
    clean[p] = '\0';
    const int n = fprintf(f,
        "JFR1\nid=%s\nstatus=%s\ndetail=%s\ngeneration=%u\nmode=%s\nfree_kb=%u\nqueue_depth=%u\n",
        id, success ? "ok" : "error", clean, (unsigned)generation,
        mode ? mode : "", (unsigned)free_kb, (unsigned)queue_depth);
    bool ok = n > 0 && fflush(f) == 0;
    if (fclose(f) != 0) ok = false;
    if (ok && rename(JF_REMOTE_RESPONSE_TMP, JF_REMOTE_RESPONSE) == 0) return true;
    // Some console filesystems disallow rename over an existing destination.
    if (ok && remove(JF_REMOTE_RESPONSE) == 0 &&
        rename(JF_REMOTE_RESPONSE_TMP, JF_REMOTE_RESPONSE) == 0) return true;
    remove(JF_REMOTE_RESPONSE_TMP);
    return false;
}

static bool read_secret(char secret[65]) {
    secret[0] = '\0';
    FILE *f = fopen(JF_REMOTE_KEY, "rb");
    if (!f) return false;
    char data[67] = "";
    const size_t n = fread(data, 1, sizeof data, f);
    fclose(f);
    if ((n != 64 && n != 65) || (n == 65 && data[64] != '\n')) return false;
    data[64] = '\0';
    if (!jf_remote_hex(data, 64)) return false;
    memcpy(secret, data, 65);
    memset(data, 0, sizeof data);
    return true;
}

void jf_remote_tick(u32 generation, const char *mode, bool playing,
                    u32 free_kb, u32 queue_depth) {
    // An old detached progress worker may still be finishing a request.
    // Never overlap file operations or block the render thread on I/O.
    RemoteIoGuard io;
    if (!io.acquired) return;
    char id[17] = "";
    char detail[128] = "";
    bool success = false;
    u32 result_gen = 0;
    bool need_write = false;
    lock_state();
    // Session teardown must not strand a command in the one-slot mailbox.
    // This is not confirmation that a remote HTTP request was rolled back.
    if ((s_state == 1 || s_state == 2) &&
        ((s_command.generation == generation && !playing) ||
         (playing && (s_command.generation != generation ||
          !mode || strcmp(s_command.mode, mode))))) {
        finish_locked(false, "playback changed before confirmation", s_command.generation);
    }
    if (s_state == 2 && timing_get_us() - s_command_started_us > 90000000ULL) {
        finish_locked(false, "Surf action timed out", generation);
    }
    if (s_state == 3) {
        snprintf(id, sizeof id, "%s", s_command.id);
        snprintf(detail, sizeof detail, "%s", s_detail);
        success = s_success;
        result_gen = s_result_generation;
        s_state = 4;
        need_write = true;
    }
    const int state = s_state;
    unlock_state();

    if (need_write) {
        const bool saved = write_result(id, success, detail, result_gen,
                                        mode, free_kb, queue_depth);
        lock_state();
        if (s_state == 4) {
            s_state = saved ? 0 : 3;
            if (saved) memset(&s_command, 0, sizeof s_command);
        }
        unlock_state();
    }
    if (state != 0 || need_write) return;

    FILE *f = fopen(JF_REMOTE_CMD, "rb");
    if (!f) return;
    char input[512];
    const size_t len = fread(input, 1, sizeof input, f);
    const bool io_error = ferror(f) != 0;
    fclose(f);
    if (io_error || !len || len == sizeof input) {
        remove(JF_REMOTE_CMD);
        return;
    }
    // If unlink fails, do not execute: the next read might replay it.
    if (remove(JF_REMOTE_CMD) != 0) return;

    JfRemoteCommand request = {};
    if (!jf_remote_parse(input, len, &request)) {
        // Bad commands are ignored; no unauthenticated response is emitted.
        return;
    }
    char secret[65] = "";
    const bool authorized = read_secret(secret) && jf_remote_auth_matches(secret, request.auth);
    memset(secret, 0, sizeof secret);
    memset(input, 0, sizeof input);
    memset(request.auth, 0, sizeof request.auth);
    // Missing/revoked/invalid key: fail closed without publishing status.
    if (!authorized) return;
    const bool query = !strcmp(request.action, "get_status") ||
                       !strcmp(request.action, "capture_metrics");
    if (!query && (request.generation == 0 || request.generation != generation)) {
        write_result(request.id, false, "playback generation mismatch",
                     generation, mode, free_kb, queue_depth);
        return;
    }
    if (!jf_remote_record_id(&s_recent_ids, request.id)) {
        write_result(request.id, false, "replayed command id",
                     generation, mode, free_kb, queue_depth);
        return;
    }
    if (query) {
        write_result(request.id, true, playing ? "playing" : "idle",
                     generation, mode, free_kb, queue_depth);
        if (!strcmp(request.action, "capture_metrics"))
            sample_owned(generation, free_kb, queue_depth, 0, 2);
        return;
    }
    if (!playing || !mode || !mode[0]) {
        write_result(request.id, false, "Surf playback is not active",
                     generation, mode, free_kb, queue_depth);
        return;
    }
    lock_state();
    if (s_state == 0) {
        snprintf(request.mode, sizeof request.mode, "%s", mode);
        s_command = request;
        s_state = 1;
    } else {
        // Defensive: if the state changed, do not stage an unsafe command.
        unlock_state();
        write_result(request.id, false, "remote controller busy",
                     generation, mode, free_kb, queue_depth);
        return;
    }
    unlock_state();
}

static void sample_owned(u32 generation, u32 free_kb, u32 queue_depth,
                         u32 audio_clock_ms, u32 flags) {
    // One 32-byte BE binary sample at a bounded cadence from progress worker.
    // Reopening every sample avoids FILE ownership across playback generations.
    if (!s_sample_initialized) {
        // Retain exactly the previous session and one bounded current archive.
        struct stat st;
        if (stat(JF_METRICS_PATH, &st) == 0 && st.st_size > 0) {
            remove(JF_METRICS_PREV);
            if (rename(JF_METRICS_PATH, JF_METRICS_PREV) != 0) {
                __sync_add_and_fetch(&s_metric_drops, 1);
                return;
            }
        }
        s_sample_initialized = true;
    }
    struct stat st;
    if (stat(JF_METRICS_PATH, &st) == 0 && st.st_size > JF_METRICS_MAX - 32) {
        remove(JF_METRICS_PREV);
        if (rename(JF_METRICS_PATH, JF_METRICS_PREV) != 0) {
            __sync_add_and_fetch(&s_metric_drops, 1);
            return; // a rotation failure must not permit unbounded growth
        }
    }
    FILE *f = fopen(JF_METRICS_PATH, "ab");
    if (!f) { __sync_add_and_fetch(&s_metric_drops, 1); return; }
    // PPU is big endian; all fields are fixed-width 32-bit integers.
    const uint32_t record[8] = {
        0x4a464d31U, (uint32_t)(generation), (uint32_t)free_kb,
        (uint32_t)queue_depth, (uint32_t)audio_clock_ms, (uint32_t)flags,
        (uint32_t)(timing_get_us() / 1000ULL), 0U
    };
    if (fwrite(record, sizeof record, 1, f) != 1)
        __sync_add_and_fetch(&s_metric_drops, 1);
    if (fclose(f) != 0) __sync_add_and_fetch(&s_metric_drops, 1);
}

void jf_remote_sample(u32 generation, u32 free_kb, u32 queue_depth,
                      u32 audio_clock_ms, u32 flags) {
    RemoteIoGuard io;
    if (!io.acquired) return;
    sample_owned(generation, free_kb, queue_depth, audio_clock_ms, flags);
    const u32 drops = __sync_lock_test_and_set(&s_metric_drops, 0);
    if (drops) {
        char line[96];
        snprintf(line, sizeof line, "jf_metrics: %u sample write failures", (unsigned)drops);
        plog(line);
    }
}

// Separate format preserves all JFM1 readers and keeps the legacy collector
// working. Only the existing progress worker calls this; no audio-thread I/O.
#define JF_METRICS2_PATH "/dev_hdd0/tmp/jf_metrics2.bin"
#define JF_METRICS2_PREV "/dev_hdd0/tmp/jf_metrics2.prev.bin"
void jf_remote_sample_extended(u32 generation, u32 free_kb, u32 queue_depth,
                               u64 audio_clock_us, u64 audio_sample_at, bool audio_valid, bool paused) {
    SurfSyncSnapshot snap = {};
    if (!surf_sync_try_snapshot(generation, &snap)) return;
    const u64 now = audio_sample_at;
    u32 blocks = 0, starves = 0;
    const bool have_counts = player_stats_audio_counts(&blocks, &starves);
    RemoteIoGuard io;
    if (!io.acquired) return;
    static bool initialized = false;
    struct stat st;
    if (stat(JF_METRICS2_PATH, &st) == 0 &&
        ((!initialized && st.st_size > 0) ||
         st.st_size > JF_METRICS_MAX - 80 || st.st_size % 80 != 0)) {
        remove(JF_METRICS2_PREV);
        if (rename(JF_METRICS2_PATH, JF_METRICS2_PREV) != 0) return;
    }
    initialized = true;
    // 20 big-endian words, 80 bytes; preserve 64-bit PTS and monotonic time.
    // Audio timestamp is sampled by the progress worker, not at the flip.
    const uint32_t record[20] = {
        0x4a464d32U, generation,
        snap.flags | (audio_valid ? 32u : 0u) | (paused ? 64u : 0u) |
        (have_counts ? 128u : 0u),
        snap.connection, snap.observed_epoch, snap.presented_seq,
        free_kb, queue_depth, blocks, starves,
        (u32)(now >> 32), (u32)now,
        (u32)(audio_clock_us >> 32), (u32)audio_clock_us,
        (u32)(snap.presented_pts_us >> 32), (u32)snap.presented_pts_us,
        (u32)(snap.presented_at_us >> 32), (u32)snap.presented_at_us,
        0u, 0u
    };
    FILE *f = fopen(JF_METRICS2_PATH, "ab");
    if (!f) { __sync_add_and_fetch(&s_metric_drops, 1); return; }
    if (fwrite(record, sizeof record, 1, f) != 1)
        __sync_add_and_fetch(&s_metric_drops, 1);
    if (fclose(f) != 0) __sync_add_and_fetch(&s_metric_drops, 1);
}
