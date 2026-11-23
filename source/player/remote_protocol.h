#pragma once
// Strict, allocation-free JFR1 wire parser. No platform dependencies.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct JfRemoteCommand {
    char id[17];
    char auth[65];
    char action[25];
    uint32_t generation;
    unsigned int value;
    bool has_value;
};

static inline bool jf_remote_hex(const char *s, size_t n) {
    if (!s || strlen(s) != n) return false;
    for (size_t i = 0; i < n; ++i)
        if (!((s[i] >= '0' && s[i] <= '9') ||
              (s[i] >= 'a' && s[i] <= 'f'))) return false;
    return true;
}

static inline bool jf_remote_action_valid(const char *s) {
    static const char *const allowed[] = {
        "get_status", "capture_metrics", "pause_surf", "resume_surf",
        "favorite_current", "select_previous", "favorite_previous",
        "select_current", "set_audio_sync_beta", "stop_playback",
    };
    if (!s) return false;
    for (size_t i = 0; i < sizeof allowed / sizeof allowed[0]; ++i)
        if (!strcmp(s, allowed[i])) return true;
    return false;
}

static inline bool jf_remote_decimal(const char *s, uint32_t *out) {
    if (!s || !*s || !out) return false;
    uint32_t v = 0;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9') return false;
        const uint32_t d = (uint32_t)(*s - '0');
        if (v > (UINT32_MAX - d) / 10U) return false;
        v = v * 10U + d;
    }
    *out = v;
    return true;
}

static inline bool jf_remote_parse(const char *src, size_t len,
                                   JfRemoteCommand *out) {
    if (!src || !out || len < 6 || len > 511 || src[len - 1] != '\n')
        return false;
    if (memcmp(src, "JFR1\n", 5)) return false;
    JfRemoteCommand parsed = {};
    unsigned int fields = 0;
    size_t pos = 5;
    while (pos < len) {
        size_t end = pos;
        while (end < len && src[end] != '\n') {
            if ((unsigned char)src[end] < 33 || (unsigned char)src[end] > 126)
                return false;
            if (end - pos >= 100) return false;
            ++end;
        }
        if (end == len || end == pos) return false;
        char line[102];
        memcpy(line, src + pos, end - pos);
        line[end - pos] = '\0';
        char *eq = strchr(line, '=');
        if (!eq || eq == line) return false;
        *eq++ = '\0';
        unsigned int bit = 0;
        if (!strcmp(line, "id")) {
            bit = 1;
            if (!jf_remote_hex(eq, 16)) return false;
            memcpy(parsed.id, eq, 17);
        } else if (!strcmp(line, "auth")) {
            bit = 2;
            if (!jf_remote_hex(eq, 64)) return false;
            memcpy(parsed.auth, eq, 65);
        } else if (!strcmp(line, "generation")) {
            bit = 4;
            if (!jf_remote_decimal(eq, &parsed.generation)) return false;
        } else if (!strcmp(line, "action")) {
            bit = 8;
            if (!jf_remote_action_valid(eq)) return false;
            const size_t size = strlen(eq);
            if (size >= sizeof parsed.action) return false;
            memcpy(parsed.action, eq, size + 1);
        } else if (!strcmp(line, "value")) {
            bit = 16;
            if (strcmp(eq, "0") && strcmp(eq, "1")) return false;
            parsed.value = (unsigned int)(eq[0] - '0');
            parsed.has_value = true;
        } else {
            return false;
        }
        if (fields & bit) return false;
        fields |= bit;
        pos = end + 1;
    }
    if ((fields & 15) != 15) return false;
    if ((!strcmp(parsed.action, "set_audio_sync_beta") && !parsed.has_value) ||
        (strcmp(parsed.action, "set_audio_sync_beta") && parsed.has_value))
        return false;
    *out = parsed;
    return true;
}

static inline bool jf_remote_auth_matches(const char *actual, const char *provided) {
    if (!actual || !provided || strlen(actual) != 64 || strlen(provided) != 64)
        return false;
    // Lengths validated; compare all bytes without an early mismatch exit.
    unsigned int mismatch = 0;
    for (size_t i = 0; i < 64; ++i)
        mismatch |= (unsigned char)actual[i] ^ (unsigned char)provided[i];
    return mismatch == 0 && actual[64] == '\0' && provided[64] == '\0';
}

// Recent command IDs provide bounded replay rejection within one app run.
// A stale authenticated command from an old playback generation is already
// blocked by the session-generation check in the mailbox.
struct JfRemoteReplay {
    char recent[16][17];
    unsigned int next;
};
static inline bool jf_remote_record_id(JfRemoteReplay *seen, const char *id) {
    if (!seen || !jf_remote_hex(id, 16)) return false;
    for (size_t i = 0; i < 16; ++i)
        if (!strcmp(seen->recent[i], id)) return false;
    memcpy(seen->recent[seen->next], id, 17);
    seen->next = (seen->next + 1U) % 16U;
    return true;
}
