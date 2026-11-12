#include "guest_range_ledger.h"
#include "rdgpu_export_token.h"
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <string.h>

static bool bounds(uintptr_t lo,size_t length,uintptr_t *hi){
    if(!lo||!length||length>UINTPTR_MAX-lo)return false;
    *hi=lo+length;return true;
}
static bool append(RDGuestRange *out,size_t *count,RDGuestRange span){
    if(span.lo>=span.hi)return true;
    if(*count==RDGUEST_RANGE_MAX)return false;
    out[(*count)++]=span;return true;
}
void rdguest_range_init(RDGuestRangeLedger *ledger){
    memset(ledger,0,sizeof *ledger);pthread_mutex_init(&ledger->lock,NULL);
}
void rdguest_range_fork_child(RDGuestRangeLedger *ledger){
    pthread_mutex_t fresh=PTHREAD_MUTEX_INITIALIZER;
    memcpy(&ledger->lock,&fresh,sizeof fresh);
}
static bool covered(const RDGuestRangeLedger *ledger,uintptr_t lo,uintptr_t hi,
                    bool requireWritable){
    uintptr_t at=lo;
    for(size_t i=0;i<ledger->count&&at<hi;i++){
        RDGuestRange s=ledger->spans[i];
        if(s.hi<=at)continue;
        if(s.lo>at||(requireWritable&&!s.writable))return false;
        at=s.hi;
    }
    return at>=hi;
}
static bool intersects(const RDGuestRangeLedger *ledger,uintptr_t lo,uintptr_t hi){
    for(size_t i=0;i<ledger->count;i++)
        if(ledger->spans[i].lo<hi&&ledger->spans[i].hi>lo)return true;
    return false;
}
static bool replace(RDGuestRangeLedger *ledger,uintptr_t lo,uintptr_t hi,
                    bool add,bool writable){
    RDGuestRange result[RDGUEST_RANGE_MAX];size_t count=0;
    for(size_t i=0;i<ledger->count;i++){
        RDGuestRange s=ledger->spans[i];
        if(s.hi<=lo||s.lo>=hi){if(!append(result,&count,s))return false;continue;}
        if(s.lo<lo&&!append(result,&count,(RDGuestRange){s.lo,lo,s.writable}))return false;
        if(s.hi>hi&&!append(result,&count,(RDGuestRange){hi,s.hi,s.writable}))return false;
    }
    if(add&&!append(result,&count,(RDGuestRange){lo,hi,writable}))return false;
    for(size_t i=1;i<count;i++){
        RDGuestRange value=result[i];size_t j=i;
        while(j&&result[j-1].lo>value.lo){result[j]=result[j-1];j--;}
        result[j]=value;
    }
    memcpy(ledger->spans,result,count*sizeof result[0]);ledger->count=count;
    return true;
}
bool rdguest_range_map(RDGuestRangeLedger *ledger,uintptr_t lo,size_t len,bool writable){
    uintptr_t hi;if(!ledger||!bounds(lo,len,&hi))return false;
    pthread_mutex_lock(&ledger->lock);
    bool ok=replace(ledger,lo,hi,true,writable);
    pthread_mutex_unlock(&ledger->lock);return ok;
}
bool rdguest_range_protect(RDGuestRangeLedger *ledger,uintptr_t lo,size_t len,bool writable){
    uintptr_t hi;if(!ledger||!bounds(lo,len,&hi))return false;
    pthread_mutex_lock(&ledger->lock);
    bool ok=covered(ledger,lo,hi,false)&&replace(ledger,lo,hi,true,writable);
    pthread_mutex_unlock(&ledger->lock);return ok;
}
bool rdguest_range_unmap(RDGuestRangeLedger *ledger,uintptr_t lo,size_t len){
    uintptr_t hi;if(!ledger||!bounds(lo,len,&hi))return false;
    pthread_mutex_lock(&ledger->lock);
    bool ok=replace(ledger,lo,hi,false,false);
    pthread_mutex_unlock(&ledger->lock);return ok;
}
bool rdguest_range_move(RDGuestRangeLedger *ledger,uintptr_t old,size_t oldLen,
                        uintptr_t target,size_t newLen){
    uintptr_t oldHi,newHi;
    if(!ledger||!bounds(old,oldLen,&oldHi)||!bounds(target,newLen,&newHi))return false;
    pthread_mutex_lock(&ledger->lock);
    bool ok=false;
    if(covered(ledger,old,oldHi,false)&&
       (newHi<=old||target>=oldHi)&&
       !intersects(ledger,target,newHi)){
        /* A move must have one uniform source permission in this small
         * ledger. Mixed protection is refused and can use map+unmap hooks. */
        bool writable=false,uniform=true,found=false;
        for(size_t i=0;i<ledger->count;i++){
            RDGuestRange s=ledger->spans[i];
            if(s.hi<=old||s.lo>=oldHi)continue;
            if(!found){writable=s.writable;found=true;}
            else if(writable!=s.writable)uniform=false;
        }
        if(uniform){
            RDGuestRange saved[RDGUEST_RANGE_MAX];size_t n=ledger->count;
            memcpy(saved,ledger->spans,n*sizeof saved[0]);
            if(replace(ledger,old,oldHi,false,false)&&
               replace(ledger,target,newHi,true,writable))ok=true;
            else {memcpy(ledger->spans,saved,n*sizeof saved[0]);ledger->count=n;}
        }
    }
    pthread_mutex_unlock(&ledger->lock);return ok;
}
bool rdguest_range_copyout(RDGuestRangeLedger *ledger,uintptr_t guestAddress,
                           const void *bytes,size_t length){
    uintptr_t hi;
    if(!ledger||!bytes||length!=sizeof(RDGPUExportImage)||
       !bounds(guestAddress,length,&hi))return false;
    pthread_mutex_lock(&ledger->lock);
    bool ok=covered(ledger,guestAddress,hi,true);
    if(ok){
        /* The same ledger lock must guard mmap/munmap/mprotect hooks to
         * prevent revocation racing this write. */
        ok=mach_vm_write(mach_task_self(),(mach_vm_address_t)guestAddress,
                         (vm_offset_t)bytes,(mach_msg_type_number_t)length)==KERN_SUCCESS;
    }
    pthread_mutex_unlock(&ledger->lock);return ok;
}
