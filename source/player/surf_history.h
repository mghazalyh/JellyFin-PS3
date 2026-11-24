#pragma once
#include <stddef.h>
#include <string.h>
#include <stdbool.h>
#include "../api/json_lite.h"
#include "surf_gate_logic.h"

// No heap, no variable-size stack arrays, and no truncated identifiers. The
// server's 8 KiB reply is checked for a closing root before any control action.
// Escaped surrogate codepoints are conservatively rejected (raw UTF-8 is fine).
static inline bool surf_token_valid(const char *token) {
    if (!token || strlen(token) != 32) return false;
    for (size_t i = 0; i < 32; ++i)
        if (hex_nibble(token[i]) < 0) return false;
    return true;
}
static inline bool surf_exact_string(const char *p, const char *end, char *out, size_t cap) {
    if (!out || !cap || !p || !end || p >= end || *p != '"') return false;
    out[0] = '\0';
    const char *q = p + 1;
    size_t bytes = 0;
    while (q < end && *q != '"') {
        unsigned char c = (unsigned char)*q++;
        if (c < 32 || c == 127) return false;
        if (c == '\\') {
            if (q == end) return false;
            c = (unsigned char)*q++;
            if (c == '"' || c == '\\' || c == '/') ++bytes;
            else if (c == 'u') {
                if (end - q < 4) return false;
                unsigned cp = 0;
                for (int j = 0; j < 4; ++j) {
                    int v = hex_nibble(q[j]);
                    if (v < 0) return false;
                    cp = (cp << 4) | (unsigned)v;
                }
                q += 4;
                if (cp < 32 || cp == 127 || (cp >= 0xd800 && cp <= 0xdfff)) return false;
                bytes += cp < 128 ? 1 : cp < 2048 ? 2 : 3;
            } else return false;
        } else ++bytes;
        if (bytes >= cap) return false;
    }
    if (q == end || bytes == 0 || cap > 2147483647u) return false;
    return read_json_string(p, q + 1, out, (int)cap) && strlen(out) == bytes;
}
static inline const char *surf_root_end(const char *json) {
    if (!json) return NULL;
    const char *end = json + strlen(json);
    const char *root = skip_ws(json, end);
    const char *close = object_end(root, end);
    return close && skip_ws(close, end) == end ? close : NULL;
}
static inline bool surf_status_matches(const char *json, const char *source_id) {
    if (!source_id || !source_id[0]) return false;
    const char *end = surf_root_end(json);
    if (!end) return false;
    const char *root = skip_ws(json, end);
    char state[24], active[257];
    return surf_exact_string(find_top_value(root,end,"state"),end,state,sizeof state) &&
           surf_exact_string(find_top_value(root,end,"source_id"),end,active,sizeof active) &&
           !strcmp(state,"playing") && !strcmp(active,source_id);
}
struct SurfHistoryEntry {
    char source[152];
    char token[40];
    char source_id[257];
};
static inline bool surf_recent_next(const char *json, const char *selected_token,
                                    char *source, size_t source_cap,
                                    char *token, size_t token_cap,
                                    char *source_id, size_t id_cap) {
    if (!source || !source_cap || !token || !token_cap || !source_id || !id_cap) return false;
    source[0] = token[0] = source_id[0] = '\0';
    const char *end = surf_root_end(json);
    if (!end) return false;
    const char *list = find_top_value(skip_ws(json,end),end,"recent_sources");
    if (!list || *list != '[') return false;
    const char *list_end = surf_json_array_end(list);
    if (!list_end || list_end >= end) return false;
    SurfHistoryEntry first = {}, item = {}, chosen = {};
    unsigned count = 0;
    bool next_after = false, have_chosen = false;
    const char *pos = skip_ws(list + 1, list_end);
    while (pos < list_end && count < 16) {
        const char *close = object_end(pos, list_end);
        if (!close) return false;  // contract: array of objects
        bool valid = surf_exact_string(find_top_value(pos,close,"source"),close,item.source,sizeof item.source) &&
                     surf_exact_string(find_top_value(pos,close,"token"),close,item.token,sizeof item.token) &&
                     surf_exact_string(find_top_value(pos,close,"source_id"),close,item.source_id,sizeof item.source_id) &&
                     surf_token_valid(item.token);
        if (valid) {
            if (count == 0) first = item;
            ++count;
            if (next_after) { chosen = item; have_chosen = true; break; }
            if (selected_token && selected_token[0] && !strcmp(selected_token,item.token)) next_after = true;
        }
        pos = skip_ws(close, list_end);
        if (pos == list_end) break;
        if (*pos != ',') return false;
        pos = skip_ws(pos + 1, list_end);
        if (pos == list_end) return false;
    }
    if (!count) return false;
    if (!have_chosen) chosen = first;
    if (strlen(chosen.source) >= source_cap || strlen(chosen.token) >= token_cap ||
        strlen(chosen.source_id) >= id_cap) return false;
    memcpy(source,chosen.source,strlen(chosen.source)+1);
    memcpy(token,chosen.token,strlen(chosen.token)+1);
    memcpy(source_id,chosen.source_id,strlen(chosen.source_id)+1);
    return true;
}
