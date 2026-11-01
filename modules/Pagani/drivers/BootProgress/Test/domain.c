// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../DomainUsage.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    odin_domain_previous previous={0};unsigned percent[2];
    uint64_t now[2][2]={{100,100},{100,100}};
    assert(!odin_domain_percent(&previous,now,1,percent));
    now[0][0]+=20;now[0][1]+=80;now[1][0]+=80;now[1][1]+=20;
    assert(odin_domain_percent(&previous,now,1,percent) && percent[0]==20 && percent[1]==80);
    assert(!odin_domain_percent(&previous,now,1,percent));
    now[0][0]=0;assert(!odin_domain_percent(&previous,now,1,percent));
    assert(!odin_domain_percent(&previous,now,0,percent));
    now[0][0]=UINT64_MAX-1;now[0][1]=UINT64_MAX-1;
    assert(!odin_domain_percent(&previous,now,1,percent));
    puts("PASS: domain deltas, first/zero sample, regression, invalidation, overflow; not MHz or UI attribution");
}
