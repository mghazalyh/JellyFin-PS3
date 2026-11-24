#pragma once
#include <ppu-types.h>
#include <stddef.h>

// A source change is observed on the progress reporter's network thread.
// A picture discontinuity is measured on the upload thread. Only the main
// display loop may confirm an actually completed flip and release the gate.
void surf_sync_session(const char *mode, u32 generation);
void surf_sync_stop(void);
void surf_sync_reconnect(const char *mode, u32 generation);
void surf_sync_health_event(const char *event, const char *reason);
void surf_sync_trace_prefix(char *out, size_t cap);
void surf_sync_set_enabled(bool enabled);
bool surf_sync_enabled(void);
void surf_sync_observe_source(const char *mode, const char *source,
                              const char *source_id, const char *boot,
                              u32 generation);
void surf_sync_probe_video(const u8 *y, u32 width, u32 height, u32 sequence,
                            u64 pts_us);
void surf_sync_display_tick(bool paused);
void surf_sync_flip_queued(u32 sequence, u64 pts_us);
void surf_sync_flip_completed(bool success);

struct SurfSyncEvent {
    char event[28];
    char mode[24];
    char source[152];
    char trace[42];
    char state[28];
};
bool surf_sync_take_event(SurfSyncEvent *out);
