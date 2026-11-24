#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../source/player/surf_gate_logic.h"
#include "../source/player/surf_history.h"
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
    puts("Surf scene gate and JSON history scanner tests passed");
    return 0;
}
