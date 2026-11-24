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
    printf("Surf state machine: %u checks, %u failures\n",checks,failures);
    return failures ? 1 : 0;
}
