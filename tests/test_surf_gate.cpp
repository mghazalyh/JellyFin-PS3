#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../source/player/surf_gate_logic.h"
int main(void) {
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
    puts("Surf scene gate host tests passed");
    return 0;
}
