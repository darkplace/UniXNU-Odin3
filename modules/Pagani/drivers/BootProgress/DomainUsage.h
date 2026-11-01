// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
// Retained accounting arithmetic from the g1-input-clusters candidate. Values
// describe busy/(busy+idle) within each pset, not MHz or UI thread attribution.
typedef struct {uint64_t time[2][2];int valid;} odin_domain_previous;
static inline int odin_domain_percent(odin_domain_previous *previous,const uint64_t now[2][2],
                                      int valid,unsigned percent[2]) {
    int ready=valid && previous->valid;
    for(unsigned d=0;d<2;++d) {
        percent[d]=0;
        if(now[d][0]<previous->time[d][0] || now[d][1]<previous->time[d][1])ready=0;
        uint64_t busy=now[d][0]-previous->time[d][0],idle=now[d][1]-previous->time[d][1];
        uint64_t total=busy+idle;
        if(!total || total<busy)ready=0;
        else if(ready)percent[d]=(unsigned)(total>UINT64_MAX/100?busy/(total/100):busy*100/total);
        for(unsigned k=0;k<2;++k)previous->time[d][k]=now[d][k];
    }
    previous->valid=valid;
    return ready;
}
