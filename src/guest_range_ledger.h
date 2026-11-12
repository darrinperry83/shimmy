#ifndef RDGUEST_RANGE_LEDGER_H
#define RDGUEST_RANGE_LEDGER_H
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define RDGUEST_RANGE_MAX 1024u
typedef struct { uintptr_t lo,hi; bool writable; } RDGuestRange;
typedef struct {
    pthread_mutex_t lock;
    size_t count;
    RDGuestRange spans[RDGUEST_RANGE_MAX];
} RDGuestRangeLedger;
void rdguest_range_init(RDGuestRangeLedger *);
void rdguest_range_fork_child(RDGuestRangeLedger *);
bool rdguest_range_map(RDGuestRangeLedger *,uintptr_t,size_t,bool writable);
bool rdguest_range_protect(RDGuestRangeLedger *,uintptr_t,size_t,bool writable);
bool rdguest_range_unmap(RDGuestRangeLedger *,uintptr_t,size_t);
bool rdguest_range_move(RDGuestRangeLedger *,uintptr_t old,size_t oldLen,
                        uintptr_t replacement,size_t newLen);
bool rdguest_range_copyout(RDGuestRangeLedger *,uintptr_t guestAddress,
                           const void *,size_t);
#endif
