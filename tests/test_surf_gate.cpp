#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../source/player/surf_gate_logic.h"
#include "../source/player/surf_history.h"
#include "../source/player/surf_display_guard.h"
static void display_clock_guard() {
    SurfDisplayGuard g = {};
    const uint64_t t = 10000000, a = 121110310, v = a + 2323000;
    assert(surf_display_guard_step(&g,true,t,a+40000,a)==SURF_DISPLAY_PASS);
    assert(surf_display_guard_step(&g,true,t,a-2300000,a)==SURF_DISPLAY_PASS);
    assert(surf_display_guard_step(&g,true,t,v,a)==SURF_DISPLAY_HOLD_BEGIN);
    for (uint64_t elapsed=10000; elapsed<2270000; elapsed+=10000)
        assert(surf_display_guard_step(&g,true,t+elapsed,v,a+elapsed)==SURF_DISPLAY_HOLD);
    assert(surf_display_guard_step(&g,true,t+2270000,v,a+2270000)==SURF_DISPLAY_RESUME);
    assert(!g.holding && !g.suppressed);
    assert(surf_display_guard_step(&g,true,t+3000000,v+3000000,a+3000000)==SURF_DISPLAY_HOLD_BEGIN);
    assert(surf_display_guard_step(&g,true,t+3400000,v+3000000,a+3000000)==SURF_DISPLAY_ABORT);
    assert(surf_display_guard_step(&g,true,t+3500000,v+3000000,a+3000000)==SURF_DISPLAY_PASS);
    assert(g.suppressed);
    assert(surf_display_guard_step(&g,true,t+4000000,a+4000000,a+4000000)==SURF_DISPLAY_PASS);
    assert(!g.suppressed);
    assert(surf_display_guard_step(&g,true,t+5000000,a+9000000,a+5000000)==SURF_DISPLAY_HOLD_BEGIN);
    for (uint64_t elapsed=100000; elapsed<2400000; elapsed+=100000)
        assert(surf_display_guard_step(&g,true,t+5000000+elapsed,a+9000000,a+5000000+elapsed)==SURF_DISPLAY_HOLD);
    assert(surf_display_guard_step(&g,true,t+7400000,a+9000000,a+7400000)==SURF_DISPLAY_ABORT);
    surf_display_guard_reset(&g);
    assert(surf_display_guard_step(&g,true,t,200000,LIVE_PTS_WRAP_US-200000)==SURF_DISPLAY_HOLD_BEGIN);
    assert(surf_display_guard_step(&g,true,t+350000,200000,150000)==SURF_DISPLAY_RESUME);
    assert(surf_display_guard_step(&g,true,t+500000,v,a)==SURF_DISPLAY_HOLD_BEGIN);
    assert(surf_display_guard_step(&g,false,t+510000,v,a)==SURF_DISPLAY_PASS && !g.holding);
    assert(surf_display_guard_step(&g,true,t+520000,v,0)==SURF_DISPLAY_PASS);
    assert(surf_display_guard_step(&g,true,t+530000,a+6000000,a)==SURF_DISPLAY_PASS);
    assert(surf_display_guard_step(&g,true,t+540000,v,a)==SURF_DISPLAY_HOLD_BEGIN);
    assert(surf_display_guard_step(&g,true,t+550000,v,a-1000)==SURF_DISPLAY_ABORT);
    surf_display_guard_reset(&g);
    assert(surf_display_guard_step(&g,true,t+560000,v,a)==SURF_DISPLAY_HOLD_BEGIN);
    assert(surf_display_guard_step(&g,true,t+550000,v,a)==SURF_DISPLAY_ABORT);
}

static void history_contract() {
    const char *json = "{\"source_id\":\"live\",\"state\":\"playing\","
        "\"recent_sources\":["
        "{\"source\":\"Sports [HD] \\\"quoted\\\"\",\"source_id\":\"s:1\",\"token\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"},"
        "{\"source\":\"Cinema \\u00e9\",\"source_id\":\"s:2\",\"token\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"}]}";
    char source[152], token[40], id[257];
    assert(surf_recent_next(json,"",source,sizeof source,token,sizeof token,id,sizeof id));
    assert(!strcmp(id,"s:1") && !strcmp(source,"Sports [HD] \"quoted\""));
    assert(surf_recent_next(json,token,source,sizeof source,token,sizeof token,id,sizeof id)==true);
    // Callers pass a distinct prior token, not the output buffer.
    assert(surf_recent_next(json,"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",source,sizeof source,token,sizeof token,id,sizeof id));
    assert(!strcmp(id,"s:2") && !strcmp(source,"Cinema \xc3\xa9"));
    assert(surf_recent_next(json,"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",source,sizeof source,token,sizeof token,id,sizeof id));
    assert(!strcmp(id,"s:1"));
    assert(!surf_recent_next(json,"",source,2,token,sizeof token,id,sizeof id));
    assert(!surf_recent_next("{\"recent_sources\":[{\"source\":\"bad\"}]","",source,sizeof source,token,sizeof token,id,sizeof id));
    assert(!surf_recent_next("{\"recent_sources\":[{\"source\":\"bad\",\"source_id\":\"bad\",\"token\":\"nothex\"}]}","",source,sizeof source,token,sizeof token,id,sizeof id));
    assert(surf_status_matches(json,"live"));
    assert(!surf_status_matches(json,"s:1"));
    assert(!surf_status_matches("{\"nested\":{\"source_id\":\"live\",\"state\":\"playing\"},\"state\":\"loading\"}","live"));
    assert(!surf_status_matches("{\"source_id\":\"live\",\"state\":\"playing\"","live"));
    assert(!surf_status_matches("{\"source_id\":\"live\\u0000other\",\"state\":\"playing\"}","live"));
    assert(!surf_status_matches("{\"source_id\":\"live\",\"state\":\"starting\"}","live"));
}
int main(void) {
    history_contract();
    display_clock_guard();
    uint8_t flat[64], other[64], small[64], half[64];
    for (int i=0;i<64;i++) {
        flat[i]=90; other[i]=200; small[i]=97;
        half[i]=(i<24)?200:90;
    }
    unsigned avg=0, changed=0;
    assert(!surf_cut_detect(NULL, flat,64,&avg,&changed));
    assert(!surf_cut_detect(flat,flat,64,&avg,&changed));
    assert(!surf_cut_detect(flat,small,64,&avg,&changed));
    assert(!surf_cut_detect(flat,half,64,&avg,&changed));
    assert(surf_cut_detect(flat,other,64,&avg,&changed));
    assert(avg==110 && changed==64);
    assert(surf_seq_reached(100,100));
    assert(surf_seq_reached(101,100));
    assert(!surf_seq_reached(99,100));
    assert(surf_seq_reached(1,0xffffffffu));
    const char *json = "[{\"source\":\"Sports [HD] \\\"quoted\\\"\",\"token\":\"abc\"},"
                       "{\"source\":\"Film [4K]\"}] tail";
    const char *end = surf_json_array_end(json);
    assert(end && end[1] == ' ' && end > json + 50);
    assert(!surf_json_array_end(NULL));
    assert(!surf_json_array_end("]"));
    assert(!surf_json_array_end("[{\"name\":\"unclosed\"}"));
    assert(!surf_json_array_end("[{\"name\":\"unclosed}]"));
    for (unsigned i = 0; i < 100000; ++i) {
        assert(surf_seq_reached(i + 1u, i));
        assert(!surf_seq_reached(i - 1u, i));
        assert(!surf_cut_detect(flat, small, 64, NULL, NULL));
    }
    puts("Surf scene gate, display clock guard and JSON history scanner tests passed");
    return 0;
}
