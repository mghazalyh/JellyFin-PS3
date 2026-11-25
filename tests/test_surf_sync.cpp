#include <stdio.h>
#include <string.h>
#include "../source/player/surf_sync.h"

static u64 now_us;
static bool gated, passthrough;
static unsigned checks, failures;
u64 timing_get_us(void) { return now_us; }
void audio_set_transition_gate(bool value) { gated = value; }
extern "C" bool audio_passthrough_active(void) { return passthrough; }
void plog(const char *) {}

#define CHECK(expr) do { ++checks; if (!(expr)) { ++failures; fprintf(stderr,"line %d: %s failed\n",__LINE__,#expr); } } while (0)
static void source(const char *id, u32 generation = 1) {
    surf_sync_observe_source("all",id,id,"boot",generation);
}
static void picture(u8 luma, u32 seq) {
    u8 y[16*16];
    memset(y,luma,sizeof y);
    surf_sync_probe_video(y,16,16,seq,seq*40000ULL);
}
static void begin() {
    now_us=1000; passthrough=false;
    surf_sync_session("all",1);
    CHECK(!gated && !surf_sync_enabled());
    surf_sync_set_enabled(true);
    source("A"); source("B"); surf_sync_display_tick(false);
    CHECK(gated);
}
// Drain the bounded event queue and report whether one state was emitted.
// A recovery timeout must be distinguishable from a confirmed flip release.
static bool take_state(const char *state) {
    SurfSyncEvent e;
    bool found = false;
    while (surf_sync_take_event(&e))
        if (!strcmp(e.state, state)) found = true;
    return found;
}
// A source observation that follows an otherwise valid candidate invalidates
// that candidate: the next source must present its own picture after gating.
static void test_source_change_after_cut() {
    begin();
    picture(90, 10);
    picture(200, 11);                  // candidate for the previous source
    source("C");                       // supersedes it before any flip
    surf_sync_flip_queued(11, 440000);
    surf_sync_flip_completed(true);
    CHECK(gated);
    CHECK(!take_state("video_flip_audio_release"));
    surf_sync_display_tick(false);
    picture(90, 12); picture(200, 13);  // C's own boundary
    surf_sync_flip_queued(13, 520000);
    surf_sync_flip_completed(true);
    CHECK(!gated);
    CHECK(take_state("video_flip_audio_release"));
}
// A source observation before the cut still gates and releases normally.
static void test_source_change_before_cut() {
    begin();
    picture(90, 30);                   // pre-transition picture
    source("D");
    surf_sync_display_tick(false);
    CHECK(gated);
    picture(90, 31);                   // first picture of D: baseline
    picture(200, 32);                  // D's cut
    surf_sync_flip_queued(32, 640000);
    surf_sync_display_tick(false);
    surf_sync_flip_completed(true);
    CHECK(!gated);
    CHECK(take_state("video_flip_audio_release"));
}
// Identical or near-identical pictures produce no cut: the bounded timeout is
// the only release, and it must be observable as such.
static void test_no_cut_timeout_and_recovery() {
    begin();
    picture(90, 40);
    for (u32 seq = 41; seq <= 45; ++seq) picture(90, seq);  // identical
    for (u32 seq = 46; seq <= 50; ++seq) picture(96, seq);  // below cut floor
    now_us = 1000 + 2999999;
    surf_sync_display_tick(false);
    CHECK(gated);
    CHECK(!take_state("gate_timeout_recovery"));
    now_us = 1000 + 3000000;
    surf_sync_display_tick(false);
    CHECK(!gated);
    CHECK(take_state("gate_timeout_recovery"));
    CHECK(!take_state("video_flip_audio_release"));
    // A new source after the timeout gets a fresh gate and can release on a
    // confirmed flip.
    source("E");
    surf_sync_display_tick(false);
    CHECK(gated);
    picture(90, 51); picture(200, 52);
    surf_sync_flip_queued(52, 1040000);
    surf_sync_flip_completed(true);
    CHECK(!gated);
    CHECK(take_state("video_flip_audio_release"));
}
// A flip completion inside the bounded window is valid evidence even when it
// arrives late; the matching submission still has to be qualified.
static void test_delayed_flip() {
    begin();
    picture(90, 60); picture(200, 61);
    surf_sync_flip_queued(61, 1220000);
    now_us = 1000 + 2500000;
    surf_sync_display_tick(false);
    CHECK(gated);
    now_us = 1000 + 2900000;
    surf_sync_flip_completed(true);
    CHECK(!gated);
    CHECK(take_state("video_flip_audio_release"));
}
// A failed completion consumes the submission once.  A later success without
// a new submission is not evidence, while a fresh submission can still release.
static void test_failed_flip() {
    begin();
    picture(90, 70); picture(200, 71);
    surf_sync_flip_queued(71, 1420000);
    surf_sync_flip_completed(false);
    CHECK(gated);
    surf_sync_flip_completed(true);
    CHECK(gated);
    surf_sync_flip_queued(71, 1420000);
    surf_sync_flip_completed(true);
    CHECK(!gated);
    CHECK(take_state("video_flip_audio_release"));
}
// Rapid source rotations while gated retain the original gate start, so the
// recovery ceiling cannot be postponed indefinitely.
static void test_rapid_switches_bound_timeout() {
    begin();
    for (int i = 0; i < 5; ++i) {
        char id[8];
        snprintf(id, sizeof id, "C%d", i);
        now_us += 500000;
        source(id);
        surf_sync_display_tick(false);
        CHECK(gated);
    }
    now_us = 1000 + 2999999;
    surf_sync_display_tick(false);
    CHECK(gated);
    CHECK(!take_state("gate_timeout_recovery"));
    now_us = 1000 + 3000000;
    surf_sync_display_tick(false);
    CHECK(!gated);
    CHECK(take_state("gate_timeout_recovery"));
}
// Time spent paused is not time spent waiting for a picture.  Resuming must
// not immediately count the pre-pause gate age as an expired recovery window.
static void test_pause_resume_excludes_paused_time() {
    begin();
    now_us = 1000 + 2000000;
    surf_sync_display_tick(true);
    CHECK(gated);
    now_us = 1000 + 5000000;
    surf_sync_display_tick(true);
    CHECK(gated);
    now_us = 1000 + 5000001;
    surf_sync_display_tick(false);
    CHECK(gated);
    CHECK(!take_state("gate_timeout_recovery"));
    now_us = 1000 + 6000000;
    surf_sync_display_tick(false);
    CHECK(gated);
    now_us = 1000 + 6000001;
    surf_sync_display_tick(false);
    CHECK(!gated);
    CHECK(take_state("gate_timeout_recovery"));
}
// Disabling, re-enabling, stopping and stale generation observations must not
// leave a gate or manufacture a release.
static void test_enable_disable_stop_and_generation() {
    begin();
    surf_sync_set_enabled(false);
    CHECK(!gated);
    CHECK(take_state("sync_beta_disabled"));
    surf_sync_set_enabled(true);
    CHECK(!gated);
    source("D");
    surf_sync_display_tick(false);
    CHECK(gated);
    surf_sync_stop();
    CHECK(!gated);
    source("E");
    surf_sync_display_tick(false);
    surf_sync_flip_completed(true);
    CHECK(!gated);

    now_us = 1000; passthrough = false;
    surf_sync_session("all", 7);
    surf_sync_set_enabled(true);
    source("A", 7);
    source("B", 6);                    // stale playback generation
    surf_sync_display_tick(false);
    CHECK(!gated);
    source("B", 7);
    surf_sync_display_tick(false);
    CHECK(gated);
    surf_sync_stop();
}
// A timeout invalidates the pending candidate; a completion from that
// superseded window cannot release anything later.
static void test_late_callback_after_timeout() {
    begin();
    picture(90, 80); picture(200, 81);
    surf_sync_flip_queued(81, 1620000);
    now_us = 1000 + 3000000;
    surf_sync_display_tick(false);
    CHECK(!gated);
    CHECK(take_state("gate_timeout_recovery"));
    surf_sync_flip_completed(true);
    CHECK(!gated);
    CHECK(!take_state("video_flip_audio_release"));
}
int main() {
    begin();
    picture(90,10); picture(200,11);
    surf_sync_flip_queued(11,440000);
    source("C");
    // A decoded candidate from a superseded source cannot release C.
    picture(90,12);
    surf_sync_flip_completed(true);
    CHECK(gated);
    surf_sync_display_tick(false);
    picture(90,13); picture(200,14);
    // Without a newly qualified flip, a completion is not evidence.
    surf_sync_flip_completed(true);
    CHECK(gated);
    surf_sync_flip_queued(14,560000);
    surf_sync_flip_completed(false);
    surf_sync_flip_completed(true);
    CHECK(gated);
    surf_sync_flip_queued(15,600000);
    surf_sync_flip_completed(true);
    CHECK(!gated);

    begin();
    now_us=2500000; source("C"); surf_sync_display_tick(false);
    CHECK(gated);
    now_us=3001000; surf_sync_display_tick(false);
    CHECK(!gated);

    begin();
    passthrough=true; source("C"); surf_sync_display_tick(false);
    CHECK(!gated);
    surf_sync_stop(); CHECK(!gated);

    begin();
    picture(90,0); picture(200,1);
    surf_sync_flip_queued(1,40000);
    surf_sync_flip_completed(true);
    CHECK(!gated);

    begin();
    picture(90,0xfffffffeu); picture(200,0xffffffffu);
    surf_sync_flip_queued(0,0);
    surf_sync_flip_completed(true);
    CHECK(!gated);

    begin();
    picture(90,20); picture(200,21);
    surf_sync_flip_queued(21,840000);
    source("stale",99);
    surf_sync_set_enabled(true);
    CHECK(gated);
    surf_sync_flip_completed(true);
    CHECK(!gated);

    begin();
    surf_sync_set_enabled(false); CHECK(!gated);
    surf_sync_session("all",2); CHECK(!gated && !surf_sync_enabled());
    surf_sync_flip_completed(true); CHECK(!gated);
    surf_sync_stop();
    now_us = 12345;
    surf_sync_session("all", 3);
    surf_sync_set_enabled(true);
    char before[24], after[24];
    surf_sync_trace_prefix(before, sizeof before);
    surf_sync_reconnect("all", 3);
    surf_sync_trace_prefix(after, sizeof after);
    CHECK(!strcmp(before, after) && surf_sync_enabled() && !gated);
    SurfSyncEvent evt;
    bool reopened = false;
    while (surf_sync_take_event(&evt)) {
        if (!strcmp(evt.state, "stream_reopened")) reopened = true;
        CHECK(!strncmp(evt.trace, before, strlen(before)));
    }
    CHECK(reopened);
    surf_sync_reconnect("sports", 3);
    CHECK(!surf_sync_enabled() && !gated);
    surf_sync_stop();
    test_source_change_after_cut();
    test_source_change_before_cut();
    test_no_cut_timeout_and_recovery();
    test_delayed_flip();
    test_failed_flip();
    test_rapid_switches_bound_timeout();
    test_pause_resume_excludes_paused_time();
    test_enable_disable_stop_and_generation();
    test_late_callback_after_timeout();

    printf("Surf state machine: %u checks, %u failures\n",checks,failures);
    return failures ? 1 : 0;
}
