#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../source/video/ts_demux.h"

void plog(const char *) {}
static TSState ts;
static u8 v[TS_VPES_BUF_SIZE], a[TS_APES_BUF_SIZE];
static int vl, al;
static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; fprintf(stderr,"line %d: %s\n",__LINE__,#x); } } while(0)
static void reset() {
    memset(&ts,0,sizeof ts);
    ts.video_pid=0x100; ts.audio_pid=0x101;
    ts_pes_stats_reset();
}
static int packet(u16 pid, unsigned cc, bool pusi, const u8 *data, int size,
                  bool disc=false, bool tei=false) {
    u8 p[188];
    memset(p,0xff,sizeof p);
    p[0]=0x47; p[1]=(u8)((pid>>8)|(pusi?0x40:0)|(tei?0x80:0)); p[2]=(u8)pid;
    if (size == 0) {
        p[3]=(u8)(0x20|(cc&15)); p[4]=183; p[5]=disc?0x80:0;
    } else if (size==184) {
        p[3]=(u8)(0x10|(cc&15)); memcpy(p+4,data,size);
    } else {
        p[3]=(u8)(0x30|(cc&15)); p[4]=(u8)(183-size);
        if(p[4]) p[5]=disc?0x80:0;
        memcpy(p+188-size,data,size);
    }
    return ts_process(&ts,p,v,&vl,a,&al);
}
static const u8 start[] = {0,0,1,0xe0,0,0,0x80,0,0,0xa1};
static const u8 tail[] = {0xb1,0xb2,0xb3};
static const u8 next[] = {0,0,1,0xe0,0,0,0x80,0,0,0xc1};

int main() {
    reset();
    CHECK(!packet(0x100,0,true,start,sizeof start));
    CHECK(!packet(0x100,2,false,tail,sizeof tail));
    CHECK(!(packet(0x100,3,true,next,sizeof next)&1));
    CHECK(packet(0x100,4,true,start,sizeof start)&1);
    CHECK(vl==(int)sizeof next && !memcmp(v,next,sizeof next));

    reset();
    CHECK(!packet(0x100,15,true,start,sizeof start));
    CHECK(!packet(0x100,0,false,tail,sizeof tail));
    CHECK(!packet(0x100,0,false,tail,sizeof tail));
    CHECK(packet(0x100,1,true,next,sizeof next)&1);
    CHECK(vl==(int)(sizeof start+sizeof tail));
    CHECK(!memcmp(v+sizeof start,tail,sizeof tail));

    reset();
    CHECK(!packet(0x100,4,true,start,sizeof start));
    CHECK(!packet(0x100,4,false,0,0,true));
    CHECK(!packet(0x100,5,false,tail,sizeof tail));
    CHECK(!(packet(0x100,6,true,next,sizeof next)&1));
    CHECK(packet(0x100,7,true,start,sizeof start)&1);
    CHECK(vl==(int)sizeof next);

    reset();
    CHECK(!packet(0x100,2,true,start,sizeof start));
    CHECK(!packet(0x100,3,false,tail,sizeof tail,false,true));
    CHECK(!(packet(0x100,4,true,next,sizeof next)&1));
    CHECK(packet(0x100,5,true,start,sizeof start)&1);

    reset();
    CHECK(!packet(0x100,2,true,start,sizeof start));
    CHECK(!packet(0x101,10,true,start,sizeof start));
    CHECK(!packet(0x100,4,false,tail,sizeof tail));
    CHECK(!packet(0x101,11,false,tail,sizeof tail));
    CHECK(packet(0x101,12,true,next,sizeof next)==2);
    CHECK(al==(int)(sizeof start+sizeof tail));
    CHECK(!(packet(0x100,5,true,next,sizeof next)&1));

    reset();
    CHECK(!packet(0x100,4,true,start,sizeof start));
    CHECK(!packet(0x100,4,false,0,0));
    CHECK(!packet(0x100,5,false,tail,sizeof tail));
    CHECK(packet(0x100,6,true,next,sizeof next)&1);
    CHECK(vl==(int)(sizeof start+sizeof tail));

    // A repeated counter with different bytes is corruption, not a duplicate.
    reset();
    CHECK(!packet(0x100,0,true,start,sizeof start));
    CHECK(!packet(0x100,1,false,tail,sizeof tail));
    CHECK(!packet(0x100,1,false,next,sizeof next));
    CHECK(!(packet(0x100,2,true,next,sizeof next)&1));
    CHECK(packet(0x100,3,true,start,sizeof start)&1);
    CHECK(vl==(int)sizeof next);
    // Discontinuity with a new start is usable immediately.
    reset();
    CHECK(!packet(0x100,6,true,start,sizeof start));
    CHECK(!packet(0x100,11,true,next,sizeof next,true));
    CHECK(packet(0x100,12,true,start,sizeof start)&1);
    CHECK(vl==(int)sizeof next);
    // Identical payload counter on a distinct PID does not interfere.
    reset();
    CHECK(!packet(0x100,0,true,start,sizeof start));
    CHECK(!packet(0x101,0,true,start,sizeof start));
    CHECK(packet(0x100,1,true,next,sizeof next)==1);
    CHECK(packet(0x101,1,true,next,sizeof next)==2);
    printf("TS continuity: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
