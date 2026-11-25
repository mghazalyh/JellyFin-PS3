#pragma once
#include <ppu-types.h>
#include "remote_protocol.h"

// All filesystem I/O is owned by the existing progress worker. The display
// thread and HTTP action worker only exchange fixed-size, synchronized data.
void jf_remote_tick(u32 generation, const char *mode, bool playing,
                    u32 free_kb, u32 queue_depth);
bool jf_remote_take(JfRemoteCommand *out);
void jf_remote_finish(const char *id, bool success, const char *detail, u32 generation);
void jf_remote_sample(u32 generation, u32 free_kb, u32 queue_depth,
                      u32 audio_clock_ms, u32 flags);
// Existing progress worker only; one bounded JFM2 snapshot every 10 seconds.
void jf_remote_sample_extended(u32 generation, u32 free_kb, u32 queue_depth,
                               u64 audio_clock_us, bool audio_valid, bool paused);
