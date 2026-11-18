#include "../src/rdgpu_guest_syscall.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static RDGPUReply transport(void *unused, const RDGPURequest *q) {
    (void)unused;
    RDGPUReply r = {.magic=RDGPU_WIRE_MAGIC,.version=RDGPU_WIRE_VERSION,
                    .bytes=RDGPU_WIRE_BYTES,.request_id=q->request_id,
                    .status=RDGPU_OK};
    if(q->op==RDGPU_CREATE_BGRA8){r.lease[0]=1;r.value1=1;}
    else {
        memcpy(r.lease,q->lease,16);
        if(q->op==RDGPU_CLEAR_DEPTH_STENCIL){
            assert((q->arg0>>40)==3);
            assert(((q->arg0>>32)&255u)==7);
            assert((uint32_t)q->arg0==0x3e800000u); /* 0.25f */
            r.value0=77;
        }
    }
    return r;
}

int main(void) {
    RDGPUGuestState state;
    rdgpu_guest_state_init(&state,getpid(),transport,NULL);
    long image=rdgpu_guest_syscall(&state,RDGPU_CREATE_BGRA8,16,16,0,0,0);
    assert(image>0);
    assert(rdgpu_guest_syscall(&state,RDGPU_GUEST_CLEAR_DEPTH_STENCIL,
                              image,0x3e800000u,7,0,0)==-EINVAL);
    assert(rdgpu_guest_syscall(&state,RDGPU_GUEST_CLEAR_DEPTH_STENCIL,
                              image,0x7fc00000u,7,3,0)==-EINVAL);
    assert(rdgpu_guest_syscall(&state,RDGPU_GUEST_CLEAR_DEPTH_STENCIL,
                              image,0x3e800000u,7,3,0)==77);
    assert(!state.slots[0].producerComplete);
    assert(rdgpu_guest_syscall(&state,RDGPU_WAIT,image,77,0,0,0)==0);
    assert(state.slots[0].producerComplete);
    puts("guest depth clear validates and carries broker lease");
    return 0;
}
