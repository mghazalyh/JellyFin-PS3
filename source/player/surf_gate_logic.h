#pragma once
#include <stddef.h>
#include <stdint.h>

// Decoder-picture scene discontinuity classifier. This does NOT identify a
// channel on its own; the caller also requires a server-observed source change
// and a completed display flip before releasing audio.
static inline bool surf_cut_detect(const uint8_t *old_luma,
                                   const uint8_t *new_luma,
                                   size_t samples,
                                   unsigned *average_out,
                                   unsigned *changed_out) {
    if (average_out) *average_out = 0;
    if (changed_out) *changed_out = 0;
    if (!old_luma || !new_luma || samples == 0) return false;
    unsigned total = 0, changed = 0;
    for (size_t i = 0; i < samples; ++i) {
        const unsigned a = old_luma[i], b = new_luma[i];
        const unsigned delta = a > b ? a - b : b - a;
        total += delta;
        if (delta >= 34) changed++;
    }
    const unsigned avg = total / (unsigned)samples;
    if (average_out) *average_out = avg;
    if (changed_out) *changed_out = changed;
    return avg >= 27 && changed * 2 >= samples;
}

static inline bool surf_seq_reached(uint32_t presented, uint32_t candidate) {
    return (int32_t)(presented - candidate) >= 0;
}


// Return the real closing bracket of a JSON array. Delimiters inside strings
// are ordinary characters (channel names often contain [HD] or escaped quotes).
// No recursion, heap allocation, or reads beyond the string terminator.
static inline const char *surf_json_array_end(const char *open) {
    if (!open || *open != '[') return 0;
    unsigned depth = 1;
    bool quoted = false, escaped = false;
    for (const char *p = open + 1; *p; ++p) {
        if (quoted && escaped) { escaped = false; continue; }
        if (quoted && *p == '\\') { escaped = true; continue; }
        if (*p == '"') { quoted = !quoted; continue; }
        if (quoted) continue;
        if (*p == '[') {
            if (depth == 32) return 0;
            ++depth;
        } else if (*p == ']' && --depth == 0) {
            return p;
        }
    }
    return 0;
}
