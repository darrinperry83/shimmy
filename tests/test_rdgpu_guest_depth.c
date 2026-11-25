#include "../src/rdgpu_guest_syscall.h"
#include "../src/rdmetal_draw_wire.h"
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
    else if(q->op==RDGPU_CREATE_DEPTH){r.lease[0]=2;r.value1=1;}
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
static long draw_transport(void *unused,const RDGPUDrawBatchRequest *batch,
                           const void *packet,size_t bytes){
    (void)unused;
    const struct rdmetal_draw_header_v5 *h=packet;
    assert(batch->packet_bytes==bytes);
    assert(h->base.base.base.base.version==RDMETAL_DRAW_VERSION_V5 ||
           h->base.base.base.base.version==RDMETAL_DRAW_VERSION_V7);
    assert(h->depth_flags==3 && h->depth_compare==1);
    return 88;
}
static int draw_poll(void *unused,uint64_t fence){
    (void)unused;assert(fence==88);return 0;
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
    long separate=rdgpu_guest_syscall(&state,RDGPU_GUEST_CREATE_DEPTH,
                                     16,16,0,0,0);
    assert(separate>0 && separate!=image && state.slots[1].isDepth);
    assert(rdgpu_guest_syscall(&state,RDGPU_GUEST_CLEAR_DEPTH_STENCIL,
                              separate,0x3e800000u,7,3,0)==77);
    assert(rdgpu_guest_syscall(&state,RDGPU_WAIT,separate,77,0,0,0)==0);
    rdgpu_guest_set_draw_transport(&state,draw_transport,draw_poll,NULL);
    struct rdmetal_draw_header_v5 draw={0};
    draw.base.base.base.base.magic=RDMETAL_DRAW_MAGIC;
    draw.base.base.base.base.version=RDMETAL_DRAW_VERSION_V5;
    draw.base.base.base.base.total_bytes=sizeof draw;
    draw.base.base.base.base.target_handle=image;
    draw.depth_handle=separate;
    draw.depth_flags=3;draw.depth_compare=1;
    assert(rdgpu_guest_syscall(&state,RDGPU_GUEST_DRAW_BATCH,image,
                              (uintptr_t)&draw,sizeof draw,0,0)==88);
    assert(rdgpu_guest_syscall(&state,RDGPU_GUEST_DRAW_POLL,image,88,0,0,0)==0);
    struct rdmetal_draw_header_v7 draw7={0};
    draw7.base.base=draw;
    ((struct rdmetal_draw_header *)&draw7)->version=RDMETAL_DRAW_VERSION_V7;
    ((struct rdmetal_draw_header *)&draw7)->total_bytes=sizeof draw7;
    draw7.color_write_mask=15;
    draw7.base.base.depth_handle=image; /* color is not a depth capability */
    assert(rdgpu_guest_syscall(&state,RDGPU_GUEST_DRAW_BATCH,image,
                              (uintptr_t)&draw7,sizeof draw7,0,0)==-EPERM);
    draw7.base.base.depth_handle=separate;
    state.slots[1].width=32;
    assert(rdgpu_guest_syscall(&state,RDGPU_GUEST_DRAW_BATCH,image,
                              (uintptr_t)&draw7,sizeof draw7,0,0)==-EPERM);
    state.slots[1].width=16;
    assert(rdgpu_guest_syscall(&state,RDGPU_GUEST_CLEAR_DEPTH_STENCIL,
                              separate,0x3e800000u,7,3,0)==77);
    assert(rdgpu_guest_syscall(&state,RDGPU_GUEST_DRAW_BATCH,image,
                              (uintptr_t)&draw7,sizeof draw7,0,0)==-EBUSY);
    assert(rdgpu_guest_syscall(&state,RDGPU_WAIT,separate,77,0,0,0)==0);
    assert(rdgpu_guest_syscall(&state,RDGPU_GUEST_DRAW_BATCH,image,
                              (uintptr_t)&draw7,sizeof draw7,0,0)==88);
    assert(rdgpu_guest_syscall(&state,RDGPU_GUEST_DRAW_POLL,image,88,0,0,0)==0);
    assert(rdgpu_guest_syscall(&state,RDGPU_RELEASE,separate,0,0,0,0)==0);
    puts("guest depth clear validates and carries broker lease");
    return 0;
}
