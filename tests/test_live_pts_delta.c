#include <stdio.h>
#include <stdint.h>
#include "../source/util/live_pts_delta.h"

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; fprintf(stderr,"line %d: %s\n",__LINE__,#x); } } while (0)
int main(void) {
    uint64_t w=LIVE_PTS_WRAP_US;
    CHECK(live_pts_delta_us(100000,140000)==-40000);
    CHECK(live_pts_delta_us(140000,100000)==40000);
    CHECK(live_pts_delta_us(20000,w-20000)==40000);
    CHECK(live_pts_delta_us(w-20000,20000)==-40000);
    CHECK(live_pts_delta_us(20000,w+20000)==0);
    CHECK(live_pts_delta_us(0,w)==0);
    CHECK(live_pts_delta_us(10000,5000000)==-4990000);
    for (uint64_t t=0;t<1000000;t+=101) {
        CHECK(live_pts_delta_us(t,w+t+33333)==-33333);
        CHECK(live_pts_delta_us(w+t+33333,t)==33333);
        CHECK(live_pts_delta_us(t,2*w+t)==0);
        CHECK(live_pts_delta_us(w-100000+t,t)==-100000);
    }
    printf("Live PTS signed rollover: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
