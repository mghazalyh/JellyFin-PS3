// AUDIO / CC track selection — opens the HUD popup menus and applies the
// chosen track.  Picking a different track reopens the stream at the current
// position with the new AudioStreamIndex/SubtitleStreamIndex.  The reopen IS
// the seek path with delta 0 — same stop-transcode + fresh-PlaySessionId
// dance, just different URL params.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "player_internal.h"
#include "plog.h"
#include "subtitles.h"
#include "slog.h"
#include "experience.h"
#include "ui_visuals.h"      // vpick_audio_words
#include "chapters.h"
#include "player_picture.h"   // picture_cycle_aspect
#include "timing.h"
#include "jellyfin_api.h"
#include "http.h"
#include "audio.h"
#include "surf_sync.h"
#include "remote_mailbox.h"
#include "surf_gate_logic.h" // bounded history parser and scene gate
#include "autoskip.h"        // Skip Intros / Skip Credits
#include "statsovl.h"        // Playback Statistics
#include "../../build_config.h"   // ENABLE_PLAYER_STATS (relative: source/ is not on the -I path)

// ---- Remembered track preference (see player_internal.h) ------------------
static char s_pref_audio_label[64] = "";
static char s_pref_sub_label[64]   = "";
static bool s_pref_sub_have        = false;   // user has chosen a sub state
static bool s_pref_sub_off         = false;   // and that choice was "Off"

void track_pref_note_audio(const JFStream *s) {
    if (!s) return;
    snprintf(s_pref_audio_label, sizeof(s_pref_audio_label), "%s", s->label);
}

void track_pref_note_sub(const JFStream *s) {
    s_pref_sub_have = true;
    s_pref_sub_off  = (s == NULL);
    if (s) snprintf(s_pref_sub_label, sizeof(s_pref_sub_label), "%s", s->label);
    else   s_pref_sub_label[0] = '\0';
}

int track_pref_find_audio(const JFTracks *tracks) {
    if (!s_pref_audio_label[0]) return -1;
    for (int i = 0; i < tracks->n_audio; i++)
        if (strcmp(tracks->audio[i].label, s_pref_audio_label) == 0)
            return i;
    return -1;
}

int track_pref_find_sub(const JFTracks *tracks) {
    if (!s_pref_sub_have || s_pref_sub_off || !s_pref_sub_label[0]) return -1;
    for (int i = 0; i < tracks->n_subs; i++)
        if (strcmp(tracks->subs[i].label, s_pref_sub_label) == 0 &&
            jf_sub_is_text(tracks->subs[i].codec))
            return i;
    return -1;
}

// ---- Chapters (R1) ----------------------------------------------------------
// The list is fetched the first time it is asked for, on a worker (chapters.h),
// so a slow server shows "Loading chapters..." and the film plays on.
static bool        s_chap_pending = false;
static char        s_chap_lab[JF_MAX_CHAPTERS][64];
static const char *s_chap_items[JF_MAX_CHAPTERS];

static double chapters_pos_secs(const PlayerState *ps) {
    return (double)(ps->play_base_us + audio_get_clock_us()) / 1e6;
}

static void chapters_banner(const char *line) {
    hud_set_banner(line, "", timing_get_us() + 2500000ULL);
}

static void chapters_open(PlayerState *ps) {
    const JFChapters *c = jf_chapters();
    for (int i = 0; i < c->n; i++) {
        jf_chapter_label(&c->ch[i], i, TR("Chapter %d"), s_chap_lab[i], sizeof s_chap_lab[i]);
        s_chap_items[i] = s_chap_lab[i];
    }
    ps->menu_kind = PLAYER_MENU_CHAPTERS;
    hud_open_menu(TR("Chapters"), s_chap_items, c->n,
                  jf_chapter_index_at(c, chapters_pos_secs(ps)));
}

// ---- Audio / Subtitles track menu ---------------------------------------------
// One menu with two lists: Triangle (and Left / Right) in it flips between them,
// and the hint bar names the other one.  Only offered when that list has entries.
static bool has_audio_menu(const PlayerState *ps) { return ps->have_tracks && ps->tracks.n_audio > 0; }
static bool has_subs_menu(const PlayerState *ps)  { return ps->have_tracks && ps->tracks.n_subs  > 0; }

static void open_audio_menu(PlayerState *ps) {
    const char *items[JF_MAX_AUDIO] = {};
    const int count = ps->tracks.n_audio < JF_MAX_AUDIO
                    ? ps->tracks.n_audio : JF_MAX_AUDIO;
    if (count <= 0) return;
    for (int i = 0; i < count; i++)
        items[i] = ps->tracks.audio[i].label;
    ps->menu_kind = PLAYER_MENU_AUDIO;
    hud_open_menu(TR("Audio"), items, count, ps->cur_audio);
    if (has_subs_menu(ps)) hud_set_menu_switch(TRN("Subtitles"));
}

static void open_subs_menu(PlayerState *ps) {
    const char *items[JF_MAX_STREAMS + 1];
    items[0] = TR("Off");
    for (int i = 0; i < ps->tracks.n_subs; i++)
        items[1 + i] = ps->tracks.subs[i].label;
    ps->menu_kind = PLAYER_MENU_SUBS;
    hud_open_menu(TR("Subtitles"), items, ps->tracks.n_subs + 1, ps->cur_sub + 1);
    if (has_audio_menu(ps)) hud_set_menu_switch(TRN("Audio"));
}

// ---- Options (START) --------------------------------------------------------
// One menu over the player, as on the console's own player: each row with what
// it is set to now.  A row that opens something (Chapters, Audio / Subtitles,
// Picture) hands over to it; a setting (Statistics, the skips) changes in place
// and the menu stays open on it; Next / Previous end this playback.
// Surf controller actions are available on the native PS3 Options menu.
// The on-screen buttons send authenticated requests through CatalogService
// to the same localhost-only Surf engine used by Jellyfin Web.
static const char *surf_mode_for(const PlayerState *ps) {
    if (!ps || !ps->live || !ps->item) return NULL;
    const char *name = ps->item->name;
    if (!strcmp(name, "Surf: All Channels")) return "all";
    if (!strcmp(name, "Surf: Sports")) return "sports";
    if (!strcmp(name, "Surf: Movies & Series")) return "movies-series";
    return NULL;
}

static bool surf_proxy_url(const char *mode, const char *action, char *url, size_t cap) {
    if (!mode || !g_server[0] || !g_token[0]) return false;
    const char *begin = strstr(g_server, "://");
    if (!begin) return false;
    begin += 3;
    const char *end = strchr(begin, '/');
    if (!end) end = begin + strlen(begin);
    const char *host_end = end;
    // If a port is specified, remove it.  Leave [IPv6] intact.
    const char *right_bracket = static_cast<const char*>(memchr(begin, ']', (size_t)(end - begin)));
    for (const char *p = begin; p < end; p++) {
        if (*p == ':' && (!right_bracket || p > right_bracket)) {
            host_end = p;
            break;
        }
    }
    const int host_len = (int)(host_end - begin);
    if (host_len <= 0 || host_len > 180) return false;
    const int n = snprintf(url, cap, "http://%.*s:8097/api/surf/%s%s%s",
                           host_len, begin, mode, action ? "/" : "",
                           action ? action : "");
    return n > 0 && (size_t)n < cap;
}

static bool surf_json_escape(const char *src, char *dest, size_t cap) {
    if (!src || !dest || cap < 2) return false;
    size_t used = 0;
    for (const unsigned char *p=(const unsigned char*)src; *p; ++p) {
        char token[3] = {0,0,0};
        if (*p == '"' || *p == '\\') { token[0]='\\'; token[1]=(char)*p; }
        else if (*p < 32) continue;
        else { token[0]=(char)*p; }
        size_t n = strlen(token);
        if (used + n + 1 > cap) return false;
        memcpy(dest + used, token, n);
        used += n;
    }
    dest[used]='\0';
    return true;
}

// An Options click is handled on the rendering thread; network IO must not be.
struct SurfActionJob {
    char base_url[320];
    char mode[24];
    char action[24];
    char source[152];
    char previous_token[40];
    char trace[32];
    char remote_id[17];
    u32 generation;
    u32 menu_epoch;
    char token[256]; // fixed-size authenticated copy, no per-click malloc
};

// A single detached worker owns this until it clears s_surf_busy.
static SurfActionJob s_surf_job;

static volatile int s_surf_busy = 0;
static volatile int s_surf_feedback_ready = 0;
static volatile u32 s_surf_feedback_generation = 0;
static volatile u32 s_surf_feedback_menu_epoch = 0;
static u32 s_surf_menu_epoch = 0;
static char s_surf_feedback_mode[24] = "";
static char s_surf_feedback_text[144] = "";
static char s_surf_result_source[152] = "";
static char s_surf_result_token[40] = "";
static bool s_surf_result_selection = false;

// Owned by the display thread. A worker publishes an immutable result then
// toggles feedback_ready; it never modifies these fields directly.
static char s_surf_selected_source[152] = "";
static char s_surf_selected_token[40] = "";
static char s_surf_selected_mode[24] = "";

void surf_menu_session_reset(void) {
    ++s_surf_menu_epoch;
    s_surf_selected_source[0] = '\0';
    s_surf_selected_token[0] = '\0';
    s_surf_selected_mode[0] = '\0';
    __sync_lock_test_and_set(&s_surf_feedback_ready, 0);
}

static bool surf_token_valid(const char *token) {
    if (!token || strlen(token) != 32) return false;
    for (int i = 0; i < 32; i++) {
        char c = token[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

static bool surf_recent_next(const char *json, const char *selected_token,
                             char *source, size_t source_cap,
                             char *token, size_t token_cap) {
    if (!json || !source || !token) return false;
    source[0] = '\0';
    token[0] = '\0';
    const char *list = strstr(json, "\"recent_sources\"");
    if (!list || !(list = strchr(list, '['))) return false;
    const char *end = surf_json_array_end(list);
    if (!end) return false;
    struct SurfHistoryEntry {
        char source[152];
        char token[40];
    } entries[16];
    int count = 0;
    for (const char *pos = list + 1; pos < end && count < 16;) {
        if (*pos != '{') { pos++; continue; }
        // Objects in recent_sources contain flat scalar fields. Strings can
        // have escaped braces, so track quote/escape state before closing.
        const char *begin = pos++;
        bool quoted = false, escaped = false;
        for (; pos < end; pos++) {
            if (escaped) { escaped = false; continue; }
            if (*pos == '\\' && quoted) { escaped = true; continue; }
            if (*pos == '"') { quoted = !quoted; continue; }
            if (*pos == '}' && !quoted) break;
        }
        if (pos >= end) break;
        const size_t n = (size_t)(pos - begin + 1);
        pos++;
        if (n > 580) continue;
        char object[584];
        memcpy(object, begin, n);
        object[n] = '\0';
        SurfHistoryEntry item = {};
        if (!json_get_string(object, "source", item.source, sizeof item.source) ||
            !json_get_string(object, "token", item.token, sizeof item.token) ||
            !item.source[0] || !surf_token_valid(item.token)) continue;
        entries[count++] = item;
    }
    if (count == 0) return false;
    int next = 0;
    if (selected_token && selected_token[0]) {
        for (int i = 0; i < count; i++)
            if (!strcmp(entries[i].token, selected_token)) {
                next = (i + 1) % count;
                break;
            }
    }
    snprintf(source, source_cap, "%s", entries[next].source);
    snprintf(token, token_cap, "%s", entries[next].token);
    return true;
}

static void surf_send_client_event(const SurfActionJob *job,
                                   const char *event, const char *source,
                                   const char *selected_source, int http_status) {
    if (!job || !event || !job->token) return;
    char url[360], body[560];
    char escaped[304] = "", selected[180] = "";
    if (!surf_json_escape(source ? source : "", escaped, sizeof escaped) ||
        !surf_json_escape(selected_source ? selected_source : "", selected, sizeof selected))
        return;
    const int n = snprintf(url, sizeof url, "%s/telemetry", job->base_url);
    if (n < 0 || (size_t)n >= sizeof url) return;
    snprintf(body, sizeof body,
             "{\"event\":\"%s\",\"source\":\"%s\",\"selected_source\":\"%s\","
             "\"trace\":\"%s\",\"status_state\":\"ps3_http_%d\"}",
             event, escaped, selected, job->trace, http_status);
    char reply[160];
    // Best effort and bounded by the HTTP helper; never alters the user action.
    const int rc = http_request(HTTP_POST, url, body,
                                job->token, reply, sizeof reply);
    if (rc < 200 || rc >= 300) {
        char buf[90];
        snprintf(buf, sizeof buf,
                 "surf_telemetry: trace=%s event=%s http=%d",
                 job->trace, event, rc);
        plog(buf);
    }
}

static void surf_action_worker(void *arg) {
    SurfActionJob *job = static_cast<SurfActionJob*>(arg);
    char reply[8192] = "";
    char payload[580] = "{}";
    char result[144] = "Surf control request failed";
    char action_url[360] = "";
    char current[152] = "";
    char new_source[152] = "";
    char new_token[40] = "";
    bool new_selection = false;
    bool selected_valid = false;
    int status = 0;
    bool valid = job && job->token[0] && job->base_url[0];
    const bool previous = valid && !strcmp(job->action, "previous");
    const bool favorite_previous = valid && !strcmp(job->action, "favorite_previous");
    const bool favorite_current = valid && !strcmp(job->action, "favorite");
    const bool select_current = valid && !strcmp(job->action, "current");
    const bool get_status = valid && (!strcmp(job->action, "status") ||
                                      previous || favorite_current);

    if (select_current) {
        snprintf(result, sizeof result, "%s", "Selection returned to Current");
        new_selection = true;
    }
    if (favorite_previous) {
        if (!job->source[0] || !surf_token_valid(job->previous_token)) {
            valid = false;
            snprintf(result, sizeof result, "%s", "Select Previous Channel first");
        } else {
            char escaped[304];
            if (!surf_json_escape(job->source, escaped, sizeof escaped)) {
                valid = false;
                snprintf(result, sizeof result, "%s", "Invalid selected channel name");
            } else {
                snprintf(payload, sizeof payload,
                    "{\"source\":\"%s\",\"previous\":true,"
                    "\"previous_token\":\"%s\",\"trace\":\"%s\"}",
                    escaped, job->previous_token, job->trace);
                snprintf(current, sizeof current, "%s", job->source);
            }
        }
    }

    if (valid && get_status) {
        status = http_request(HTTP_GET, job->base_url, NULL,
                              job->token, reply, sizeof reply);
        if (status != 200) {
            valid = false;
            snprintf(result, sizeof result,
                     "Surf status unavailable (HTTP %d)", status);
            surf_send_client_event(job, "status_error", "", "", status);
        } else if (previous) {
            if (surf_recent_next(reply, job->previous_token,
                                 new_source, sizeof new_source,
                                 new_token, sizeof new_token)) {
                snprintf(result, sizeof result, "Previous: %.124s", new_source);
                new_selection = true;
                selected_valid = true;
                surf_send_client_event(job, "previous_clicked",
                                       "", new_source, status);
            } else {
                snprintf(result, sizeof result, "%s", "No previous playable channel");
                valid = false;
                surf_send_client_event(job, "previous_missing", "", "", status);
            }
        } else if (!json_get_string(reply, "source", current, sizeof current) ||
                   !current[0]) {
            valid = false;
            snprintf(result, sizeof result, "%s", "No current playable source");
        } else if (!strcmp(job->action, "status")) {
            snprintf(result, sizeof result, "Surf sending: %.124s", current);
        } else {
            char escaped[304];
            if (!surf_json_escape(current, escaped, sizeof escaped)) {
                valid = false;
                snprintf(result, sizeof result, "%s", "Invalid source name");
            } else {
                snprintf(payload, sizeof payload,
                    "{\"source\":\"%s\",\"trace\":\"%s\"}",
                    escaped, job->trace);
            }
        }
    }

    const bool post_action = valid && !get_status &&
                              !select_current && !previous &&
                              strcmp(job->action, "status") != 0;
    const bool post_favorite = valid && (favorite_previous || favorite_current);
    // Favoriting takes priority over best-effort telemetry. Posting telemetry
    // first can allow the server to rotate before the requested favorite.

    if (post_action || (valid && favorite_current)) {
        const char *method = favorite_previous ? "favorite" : job->action;
        const int n = snprintf(action_url, sizeof action_url,
                               "%s/%s", job->base_url, method);
        if (n <= 0 || (size_t)n >= sizeof action_url) {
            snprintf(result, sizeof result, "%s", "Surf URL is too long");
            status = 0;
        } else {
            reply[0] = '\0';
            status = http_request(HTTP_POST, action_url, payload,
                                  job->token, reply, sizeof reply);
            if (status >= 200 && status < 300) {
                const char *message =
                    !strcmp(job->action, "pause")
                    ? "Looping paused; playback continues"
                    : !strcmp(job->action, "resume")
                    ? "Automatic looping resumed"
                    : favorite_previous
                    ? "Selected Previous channel favorited"
                    : "Current server source favorited";
                snprintf(result, sizeof result, "%s", message);
            } else {
                snprintf(result, sizeof result,
                         "Surf %s failed (HTTP %d)", method, status);
            }
        }
        if (post_favorite) {
            surf_send_client_event(job, "favorite_clicked", current,
                                   favorite_previous ? job->source : "", status);
            surf_send_client_event(job,
                status >= 200 && status < 300 ? "favorite_confirmed" : "favorite_failed",
                current, favorite_previous ? job->source : "", status);
        }
    }

    if (job) {
        snprintf(s_surf_feedback_mode, sizeof s_surf_feedback_mode, "%s",
                 job->mode);
        s_surf_feedback_generation = job->generation;
        s_surf_feedback_menu_epoch = job->menu_epoch;
        // Clear the duplicate authentication data before allowing reuse.
        memset(job->token, 0, sizeof job->token);
    }
    snprintf(s_surf_feedback_text, sizeof s_surf_feedback_text, "%s", result);
    snprintf(s_surf_result_source, sizeof s_surf_result_source, "%s",
             selected_valid ? new_source : "");
    snprintf(s_surf_result_token, sizeof s_surf_result_token, "%s",
             selected_valid ? new_token : "");
    s_surf_result_selection = new_selection;
    __sync_synchronize();
    __sync_lock_test_and_set(&s_surf_feedback_ready, 1);
    if (job && job->remote_id[0]) {
        const bool ok = valid && (select_current || previous ||
                        (!strcmp(job->action, "status") && status == 200) ||
                        ((post_action || favorite_current) &&
                         status >= 200 && status < 300));
        jf_remote_finish(job->remote_id, ok, result, job->generation);
    }
    __sync_lock_release(&s_surf_busy);
    sysThreadExit(0);
}

// Only the rendering thread publishes selection to the HUD/menu. Its identity
// is stable across a Surf rotation and never follows a changing status field.
static void surf_feedback_poll(const PlayerState *ps) {
    if (!__sync_lock_test_and_set(&s_surf_feedback_ready, 0)) return;
    __sync_synchronize();
    const char *mode = surf_mode_for(ps);
    if (!mode || strcmp(mode, s_surf_feedback_mode) ||
        s_surf_feedback_generation != g_prog.gen ||
        s_surf_feedback_menu_epoch != s_surf_menu_epoch) return;
    if (s_surf_result_selection) {
        snprintf(s_surf_selected_source, sizeof s_surf_selected_source, "%s",
                 s_surf_result_source);
        snprintf(s_surf_selected_token, sizeof s_surf_selected_token, "%s",
                 s_surf_result_token);
        snprintf(s_surf_selected_mode, sizeof s_surf_selected_mode, "%s",
                 mode);
    }
    hud_set_banner("Surf Live TV", s_surf_feedback_text,
                   timing_get_us() + 3500000ULL);
}

static bool surf_action(PlayerState *ps, const char *action,
                        const char *remote_id = NULL) {
    const char *mode = surf_mode_for(ps);
    if (!mode || !action) return false;
    if (!__sync_bool_compare_and_swap(&s_surf_busy, 0, 1)) {
        hud_set_banner("Surf Live TV", "Surf request is in progress",
                       timing_get_us() + 2000000ULL);
        return false;
    }
    SurfActionJob *job = &s_surf_job;
    memset(job, 0, sizeof *job);
    snprintf(job->mode, sizeof job->mode, "%s", mode);
    snprintf(job->action, sizeof job->action, "%s", action);
    if (remote_id) snprintf(job->remote_id, sizeof job->remote_id, "%s", remote_id);
    job->generation = g_prog.gen;
    job->menu_epoch = s_surf_menu_epoch;
    snprintf(job->trace, sizeof job->trace, "ps3_%08x_%08x",
             (unsigned)timing_get_us(), (unsigned)ps->frame_count);
    if (strcmp(s_surf_selected_mode, mode) != 0) {
        s_surf_selected_source[0] = '\0';
        s_surf_selected_token[0] = '\0';
        snprintf(s_surf_selected_mode, sizeof s_surf_selected_mode, "%s", mode);
    }
    snprintf(job->source, sizeof job->source, "%s", s_surf_selected_source);
    snprintf(job->previous_token, sizeof job->previous_token, "%s",
             s_surf_selected_token);
    if (!surf_proxy_url(mode, NULL, job->base_url, sizeof job->base_url)) {
        __sync_lock_release(&s_surf_busy);
        hud_set_banner("Surf Live TV", "Surf server address unavailable",
                       timing_get_us() + 3000000ULL);
        return false;
    }
    snprintf(job->token, sizeof job->token, "%s", g_token);
    if (!job->token[0]) {
        __sync_lock_release(&s_surf_busy);
        hud_set_banner("Surf Live TV", "Jellyfin authentication unavailable",
                       timing_get_us() + 2000000ULL);
        return false;
    }
    sys_ppu_thread_t tid;
    static char name[] = "jf_surf_ctl";
    const int rc = sysThreadCreate(&tid, surf_action_worker, job,
                                   1600, 0x10000, 0, name);
    if (rc != 0) {
        memset(job->token, 0, sizeof job->token);
        __sync_lock_release(&s_surf_busy);
        hud_set_banner("Surf Live TV", "Cannot start Surf network worker",
                       timing_get_us() + 3000000ULL);
        return false;
    }
    hud_set_banner("Surf Live TV", "Sending Surf request...",
                   timing_get_us() + 1800000ULL);
    return true;
}

// The remote mailbox uses exactly the same authenticated, generation-scoped
// action worker as the physical START menu. No duplicate HTTP implementation.
bool player_remote_surf_action(PlayerState *ps, const char *action, const char *id) {
    if (!ps || !action || !id) return false;
    if (!strcmp(action, "pause_surf")) return surf_action(ps, "pause", id);
    if (!strcmp(action, "resume_surf")) return surf_action(ps, "resume", id);
    if (!strcmp(action, "favorite_current")) return surf_action(ps, "favorite", id);
    if (!strcmp(action, "select_previous")) return surf_action(ps, "previous", id);
    if (!strcmp(action, "favorite_previous")) return surf_action(ps, "favorite_previous", id);
    if (!strcmp(action, "select_current")) return surf_action(ps, "current", id);
    return false;
}

enum { OPT_CHAPTERS, OPT_TRACKS, OPT_PICTURE, OPT_STATS, OPT_NEXT, OPT_PREV,
       OPT_SKIP_INTRO, OPT_SKIP_CREDITS,
       OPT_SURF_PAUSE, OPT_SURF_RESUME, OPT_SURF_FAVORITE,
       OPT_SURF_PREVIOUS, OPT_SURF_FAVORITE_PREVIOUS, OPT_SURF_CURRENT,
       OPT_SURF_STATUS, OPT_SURF_SYNC, OPT_MAX };
static int         s_opt_kind[OPT_MAX];
static const char *s_opt_items[OPT_MAX];
static const char *s_opt_vals[OPT_MAX];
static char        s_opt_valbuf[OPT_MAX][96];
static int         s_opt_n;

static bool is_episode(const PlayerState *ps) {
    return ps->item && strcmp(ps->item->type, "Episode") == 0;
}

static void opt_chapters_value(const PlayerState *ps, char *out, int cap) {
    const int st = jf_chapters_state();
    if (st == JF_CHAPTERS_READY) {
        const JFChapters *c = jf_chapters();
        const int i = jf_chapter_index_at(c, chapters_pos_secs(ps));
        if (i < 0)               snprintf(out, cap, "%s", "");
        else if (c->ch[i].name[0]) snprintf(out, cap, "%s", c->ch[i].name);
        else                     snprintf(out, cap, TR("Chapter %d"), i + 1);
    } else if (st == JF_CHAPTERS_NONE) {
        snprintf(out, cap, "%s", TR("No chapters"));
    } else {
        snprintf(out, cap, "%s", TR("Loading chapters..."));
    }
}

static void opt_tracks_value(const PlayerState *ps, char *out, int cap) {
    char a[64] = "";
    if (ps->cur_audio >= 0 && ps->cur_audio < ps->tracks.n_audio)
        vpick_audio_words(ps->tracks.audio[ps->cur_audio].label, a, sizeof a);
    const char *s = (ps->cur_sub >= 0 && ps->cur_sub < ps->tracks.n_subs)
                  ? ps->tracks.subs[ps->cur_sub].label : TR("Off");
    snprintf(out, cap, "%s / %s", a[0] ? a : TR("Default"), s);
}

void player_open_options(PlayerState *ps, int row) {
    int n = 0;
    const bool chapters = player_has_chapters(ps);
    const bool tracks   = !ps->live && (has_audio_menu(ps) || has_subs_menu(ps));
    const bool ep       = is_episode(ps);
    if (chapters) {
        s_opt_kind[n] = OPT_CHAPTERS; s_opt_items[n] = TR("Chapters");
        opt_chapters_value(ps, s_opt_valbuf[n], sizeof s_opt_valbuf[n]); n++;
    }
    if (tracks) {
        s_opt_kind[n] = OPT_TRACKS; s_opt_items[n] = TR("Audio / Subtitles");
        opt_tracks_value(ps, s_opt_valbuf[n], sizeof s_opt_valbuf[n]); n++;
    }
    {
        const int a = picture_get().aspect;
        s_opt_kind[n] = OPT_PICTURE; s_opt_items[n] = TR("Picture");
        snprintf(s_opt_valbuf[n], sizeof s_opt_valbuf[n], "%s",
                 a == PIC_FILL ? TR("Fill") : a == PIC_STRETCH ? TR("Stretch") : TR("Fit"));
        n++;
    }
#if ENABLE_PLAYER_STATS
    s_opt_kind[n] = OPT_STATS; s_opt_items[n] = TR("Playback Statistics");
    snprintf(s_opt_valbuf[n], sizeof s_opt_valbuf[n], "%s", statsovl_shown() ? TR("On") : TR("Off"));
    n++;
#endif
    if (!ps->live && ps->has_next) {
        s_opt_kind[n] = OPT_NEXT; s_opt_items[n] = ep ? TR("Next Episode") : TR("Next");
        snprintf(s_opt_valbuf[n], sizeof s_opt_valbuf[n], "%s", ps->next_name); n++;
    }
    if (!ps->live && ps->has_prev) {
        s_opt_kind[n] = OPT_PREV; s_opt_items[n] = ep ? TR("Previous Episode") : TR("Previous");
        snprintf(s_opt_valbuf[n], sizeof s_opt_valbuf[n], "%s", ps->prev_name); n++;
    }
    if (!ps->live && ep) {
        s_opt_kind[n] = OPT_SKIP_INTRO; s_opt_items[n] = TR("Skip Intros");
        snprintf(s_opt_valbuf[n], sizeof s_opt_valbuf[n], "%s", tr(skip_mode_label(skip_intro_mode()))); n++;
        s_opt_kind[n] = OPT_SKIP_CREDITS; s_opt_items[n] = TR("Skip Credits");
        snprintf(s_opt_valbuf[n], sizeof s_opt_valbuf[n], "%s", tr(skip_mode_label(skip_credits_mode()))); n++;
    }
    if (surf_mode_for(ps)) {
        s_opt_kind[n] = OPT_SURF_PAUSE; s_opt_items[n] = "Pause Surf Looping";
        snprintf(s_opt_valbuf[n], sizeof s_opt_valbuf[n], "Hold this channel"); n++;
        s_opt_kind[n] = OPT_SURF_RESUME; s_opt_items[n] = "Resume Surf Looping";
        snprintf(s_opt_valbuf[n], sizeof s_opt_valbuf[n], "Switch each minute"); n++;
        s_opt_kind[n] = OPT_SURF_FAVORITE; s_opt_items[n] = "Favorite Current Channel";
        snprintf(s_opt_valbuf[n], sizeof s_opt_valbuf[n], "Server source, not buffered picture"); n++;
        s_opt_kind[n] = OPT_SURF_PREVIOUS; s_opt_items[n] = "Select Previous Channel";
        snprintf(s_opt_valbuf[n], sizeof s_opt_valbuf[n], "%s",
                 s_surf_selected_source[0] ? s_surf_selected_source : "Cycle playable history"); n++;
        s_opt_kind[n] = OPT_SURF_FAVORITE_PREVIOUS;
        s_opt_items[n] = "Favorite Selected Previous";
        snprintf(s_opt_valbuf[n], sizeof s_opt_valbuf[n], "%s",
                 s_surf_selected_source[0] ? s_surf_selected_source : "Select Previous first"); n++;
        s_opt_kind[n] = OPT_SURF_CURRENT; s_opt_items[n] = "Select Current Channel";
        snprintf(s_opt_valbuf[n], sizeof s_opt_valbuf[n], "Clear Previous selection"); n++;
        s_opt_kind[n] = OPT_SURF_STATUS; s_opt_items[n] = "Show Surf Source Status";
        snprintf(s_opt_valbuf[n], sizeof s_opt_valbuf[n], "Publisher state may lead screen"); n++;
        s_opt_kind[n] = OPT_SURF_SYNC; s_opt_items[n] = "Adaptive Audio Sync (Beta)";
        snprintf(s_opt_valbuf[n], sizeof s_opt_valbuf[n], "%s",
                 surf_sync_enabled() ? "On (experimental)" : "Off by default"); n++;
    }
    for (int i = 0; i < n; i++) s_opt_vals[i] = s_opt_valbuf[i];
    s_opt_n = n;
    ps->menu_kind = PLAYER_MENU_OPTIONS;
    hud_open_menu(TR("Options"), s_opt_items, n, -1);
    hud_set_menu_values(s_opt_vals);
    hud_set_menu_cursor(row);
}

// X on an Options row.  Returns the action still to perform.
static HudAction options_select(PlayerState *ps, int sel) {
    ps->menu_kind = PLAYER_MENU_NONE;
    if (sel < 0 || sel >= s_opt_n) return HUD_ACTION_NONE;
    switch (s_opt_kind[sel]) {
    case OPT_CHAPTERS:  return player_handle_menu_action(ps, HUD_ACTION_CHAPTERS);
    case OPT_TRACKS:    return player_handle_menu_action(ps, HUD_ACTION_AUDIO_TRACK);
    case OPT_PICTURE:   picture_mode_open(); return HUD_ACTION_NONE;
    case OPT_NEXT:      return HUD_ACTION_NEXT_ITEM;
    case OPT_PREV:      return HUD_ACTION_PREV_ITEM;
#if ENABLE_PLAYER_STATS
    case OPT_STATS:     statsovl_toggle_shown(); break;          // this playback, as R3 does
#endif
    case OPT_SKIP_INTRO:   skip_intro_step(+1);   break;
    case OPT_SKIP_CREDITS: skip_credits_step(+1); break;
    case OPT_SURF_PAUSE:   surf_action(ps,"pause");   break;
    case OPT_SURF_RESUME:  surf_action(ps,"resume");  break;
    case OPT_SURF_FAVORITE:          surf_action(ps,"favorite"); break;
    case OPT_SURF_PREVIOUS:          surf_action(ps,"previous"); break;
    case OPT_SURF_FAVORITE_PREVIOUS: surf_action(ps,"favorite_previous"); break;
    case OPT_SURF_CURRENT:           surf_action(ps,"current"); break;
    case OPT_SURF_STATUS:            surf_action(ps,"status"); break;
    case OPT_SURF_SYNC:              surf_sync_set_enabled(!surf_sync_enabled()); break;
    default: return HUD_ACTION_NONE;
    }
    player_open_options(ps, sel);          // a setting: stay, on the same row
    return HUD_ACTION_NONE;
}

HudAction player_handle_menu_action(PlayerState *ps, HudAction act) {
    surf_feedback_poll(ps);
    if (act == HUD_ACTION_MENU_SWITCH) {
        act = HUD_ACTION_NONE;
        if (ps->menu_kind == PLAYER_MENU_AUDIO && has_subs_menu(ps))       open_subs_menu(ps);
        else if (ps->menu_kind == PLAYER_MENU_SUBS && has_audio_menu(ps))  open_audio_menu(ps);
        return act;
    }
    if (s_chap_pending) {
        const int st = jf_chapters_state();
        if (st == JF_CHAPTERS_READY)     { s_chap_pending = false; chapters_open(ps); }
        else if (st == JF_CHAPTERS_NONE) { s_chap_pending = false; chapters_banner(TR("No chapters")); }
        else if (st == JF_CHAPTERS_IDLE) { s_chap_pending = false; }     // another film started
    }
    if (act == HUD_ACTION_ASPECT) {
        picture_mode_open();
        return HUD_ACTION_NONE;
    }
    if (act == HUD_ACTION_CHAPTERS) {
        act = HUD_ACTION_NONE;
        const int st = jf_chapters_state();
        if (!player_has_chapters(ps)) {
            chapters_banner(TR("No chapters"));          // a file on a drive: not read from the file yet
        } else if (st == JF_CHAPTERS_READY) {
            chapters_open(ps);
        } else if (st == JF_CHAPTERS_NONE) {
            chapters_banner(TR("No chapters"));
        } else {
            if (st == JF_CHAPTERS_IDLE) jf_chapters_request(ps->item->id);
            s_chap_pending = true;
            chapters_banner(TR("Loading chapters..."));
        }
        return act;
    }
    if (act == HUD_ACTION_AUDIO_TRACK) {
        act = HUD_ACTION_NONE;
        if (has_audio_menu(ps)) {
            open_audio_menu(ps);
        } else if (has_subs_menu(ps)) {
            open_subs_menu(ps);              // no audio list to show: Triangle still reaches the subtitles
        } else {
            plog("hud: audio - no tracks");
        }
    } else if (act == HUD_ACTION_SUBTITLE) {
        act = HUD_ACTION_NONE;
        if (has_subs_menu(ps)) {
            open_subs_menu(ps);
        } else {
            plog("hud: subs - none available");
            chapters_banner(TR("No subtitles"));   // say so, rather than the press doing nothing
        }
    } else if (act == HUD_ACTION_MENU_SELECT) {
        act = HUD_ACTION_NONE;
        int sel = hud_menu_choice();
        if (ps->menu_kind == PLAYER_MENU_OPTIONS) return options_select(ps, sel);
        if (ps->menu_kind == PLAYER_MENU_CHAPTERS) {
            const JFChapters *c = jf_chapters();
            if (jf_chapters_state() == JF_CHAPTERS_READY && sel >= 0 && sel < c->n) {
                const double to = c->ch[sel].start_secs, from = chapters_pos_secs(ps);
                const int delta = (int)(to >= from ? to - from + 0.5 : to - from - 0.5);
                char b[80];
                snprintf(b, sizeof b, "hud: chapter %d -> %.0f s (%+d)", sel + 1, to, delta);
                plog(b);
                if (delta != 0) player_seek_queue_tap(ps, delta);
            }
            ps->menu_kind = PLAYER_MENU_NONE;
            return HUD_ACTION_NONE;
        }
        if (ps->menu_kind == PLAYER_MENU_AUDIO &&
            sel >= 0 && sel < ps->tracks.n_audio && sel != ps->cur_audio) {
            ps->cur_audio = sel;
            track_pref_note_audio(&ps->tracks.audio[ps->cur_audio]);
            {
                // The audio chip names the track, in the selector's words.
                char w[64];
                vpick_audio_words(ps->tracks.audio[ps->cur_audio].label, w, sizeof w);
                hud_set_audio_label(w[0] ? w : TR("Default"));
            }
            act = HUD_ACTION_SEEK;     // 0-delta reopen applies the track
            char buf[96];
            snprintf(buf, sizeof(buf), "hud: audio -> [%d] %s",
                     ps->tracks.audio[ps->cur_audio].index,
                     ps->tracks.audio[ps->cur_audio].label);
            plog(buf);
            slog_state("AUDIO_TRACK sel=%d idx=%d label=%.30s",
                       ps->cur_audio, ps->tracks.audio[ps->cur_audio].index,
                       ps->tracks.audio[ps->cur_audio].label);
        } else if (ps->menu_kind == PLAYER_MENU_SUBS &&
                   sel >= 0 && sel <= ps->tracks.n_subs &&
                   sel - 1 != ps->cur_sub) {
            ps->cur_sub = sel - 1;     // entry 0 = "Off" -> -1
            hud_set_cc_active(ps->cur_sub >= 0);

            // A file on a drive: the cues come out of the file as it is read, so a track change is the 0-delta
            // reopen (player_local_open starts collecting the new track); Off needs none, the cues just stop being kept.
            if (ps->local) {
                ps->sub_is_text = ps->sub_is_pgs = false;
                if (ps->cur_sub >= 0 && local_is_sidecar(ps->tracks.subs[ps->cur_sub].index)) {
                    // A file beside the video: read whole, no reopen needed for it.  A Matroska file reading an
                    // embedded track is reopened without it, or its cues would join the file's.
                    ps->sub_is_text = player_local_load_sidecar(ps, ps->cur_sub);
                    if (!ps->sub_is_text) { ps->cur_sub = -1; hud_set_cc_active(false); chapters_banner(TR("No subtitles")); }
                    if (ps->local->container == LM_MKV) act = HUD_ACTION_SEEK;
                    ps->menu_kind = PLAYER_MENU_NONE;
                    return act;
                }
                if (ps->cur_sub >= 0) {
                    const JFStream *st = &ps->tracks.subs[ps->cur_sub];
                    ps->sub_is_text = jf_sub_is_text(st->codec);
                    ps->sub_is_pgs  = jf_sub_is_pgs(st->codec);
                    act = HUD_ACTION_SEEK;
                    char b[96];
                    snprintf(b, sizeof(b), "hud: subs (local) -> [%d] %.40s", st->index, st->label);
                    plog(b);
                } else {
                    subs_clear();
                    plog("hud: subs (local) -> off");
                }
                ps->menu_kind = PLAYER_MENU_NONE;
                return act;
            }

            // A TEXT or PGS track is fetched and drawn here, so it needs no
            // reopen at all -- the video stream is untouched and carries on.
            // Only a track this app cannot decode itself (VOBSUB, or a
            // fetch failure) changes the URL to ask for burn-in, and only
            // that case is worth interrupting playback for.
            ps->sub_is_text = false;
            ps->sub_is_pgs  = false;
            if (ps->cur_sub >= 0) {
                const JFStream *st = &ps->tracks.subs[ps->cur_sub];
                ps->sub_is_text = jf_sub_is_text(st->codec);
                if (ps->sub_is_text) {
                    const char *msid = ps->source.id;
                    if (subs_load(ps->item->id, msid, st->index) > 0) {
                        ps->menu_kind = PLAYER_MENU_NONE;
                        track_pref_note_sub(st);
                        char b[112];
                        snprintf(b, sizeof(b),
                                 "subs: drawing [%d] %.40s on-device, no reopen",
                                 st->index, st->label);
                        plog(b);
                        return act;            // nothing to re-negotiate
                    }
                    // Could not fetch it: fall through to the burn-in path
                    // rather than silently showing nothing.
                    plog("subs: on-device load failed, asking the server to burn in");
                    ps->sub_is_text = false;
                } else if (jf_sub_is_pgs(st->codec)) {
                    const char *msid = ps->source.id;
                    if (subs_load_pgs(ps->item->id, msid, st->index) > 0) {
                        ps->sub_is_pgs = true;
                        ps->menu_kind  = PLAYER_MENU_NONE;
                        // Not remembered via track_pref_note_sub(): that
                        // preference match requires jf_sub_is_text() (see
                        // player_internal.h), so a PGS pick is deliberately
                        // never auto-applied to the next title -- it would
                        // have to fetch a whole .sup speculatively to find
                        // out whether the match was even right.
                        char b[112];
                        snprintf(b, sizeof(b),
                                 "subs: drawing pgs [%d] %.40s on-device, no reopen",
                                 st->index, st->label);
                        plog(b);
                        return act;
                    }
                    plog("subs: pgs load failed, asking the server to burn in");
                }
            } else {
                subs_clear();
                track_pref_note_sub(NULL);
            }
            act = HUD_ACTION_SEEK;     // 0-delta reopen applies the sub
            char buf[96];
            if (ps->cur_sub >= 0)
                snprintf(buf, sizeof(buf), "hud: subs -> [%d] %s",
                         ps->tracks.subs[ps->cur_sub].index,
                         ps->tracks.subs[ps->cur_sub].label);
            else
                snprintf(buf, sizeof(buf), "hud: subs -> off");
            plog(buf);
            if (ps->cur_sub >= 0)
                slog_state("SUB_TRACK sel=%d idx=%d label=%.30s", ps->cur_sub,
                           ps->tracks.subs[ps->cur_sub].index,
                           ps->tracks.subs[ps->cur_sub].label);
            else
                slog_state("SUB_TRACK sel=-1 off=1");
        }
        ps->menu_kind = PLAYER_MENU_NONE;
    }
    return act;
}
