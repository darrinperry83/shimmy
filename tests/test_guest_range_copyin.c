#include "../src/guest_range_ledger.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void){
    RDGuestRangeLedger ledger;
    rdguest_range_init(&ledger);
    uint8_t guest[64],snapshot[64];
    for(unsigned i=0;i<sizeof guest;i++)guest[i]=(uint8_t)(i+1);
    memset(snapshot,0,sizeof snapshot);
    assert(!rdguest_range_copyin(&ledger,(uintptr_t)guest,snapshot,64));
    assert(rdguest_range_map(&ledger,(uintptr_t)guest,64,false));
    assert(rdguest_range_copyin(&ledger,(uintptr_t)guest,snapshot,64));
    assert(!memcmp(guest,snapshot,64));
    assert(!rdguest_range_copyin(&ledger,(uintptr_t)guest+32,snapshot,64));
    assert(!rdguest_range_copyin(&ledger,UINTPTR_MAX-3,snapshot,64));
    assert(rdguest_range_unmap(&ledger,(uintptr_t)guest,64));
    assert(!rdguest_range_copyin(&ledger,(uintptr_t)guest,snapshot,64));
    puts("PASS guest-owned copyin admission, bounds and revocation");
    return 0;
}
