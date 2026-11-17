#include "../src/rdgpu_guest_syscall.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int begins, completions, exports;
static RDGPUReply transport(void *context,const RDGPURequest *q) {
    (void)context;
    RDGPUReply r={.magic=RDGPU_WIRE_MAGIC,.version=RDGPU_WIRE_VERSION,
        .bytes=RDGPU_WIRE_BYTES,.request_id=q->request_id,.status=RDGPU_OK};
    if(q->op==RDGPU_CREATE_BGRA8){r.lease[0]=1;r.value1=1;}
    else {memcpy(r.lease,q->lease,16);
          if(q->op==RDGPU_CLEAR)r.value0=77;}
    return r;
}
static long stride(void *ctx,pid_t pid,const uint8_t lease[16],uint64_t gen,
                   uint32_t width,uint32_t height){
    (void)ctx;assert(pid==getpid()&&lease[0]==1&&gen==1);
    assert(width==16&&height==16);return 64;
}
static long issue(void *ctx,pid_t pid,const uint8_t lease[16],uint64_t gen,
                  uint64_t handle,uint64_t fence,uint32_t width,
                  uint32_t height,uint32_t bytes){
    (void)ctx;(void)handle;
    assert(pid==getpid()&&lease[0]==1&&gen==1&&fence==0);
    assert(width==16&&height==16&&bytes==64);
    exports++;return 123;
}
static int begin(void *ctx,pid_t pid,const uint8_t lease[16],uint64_t gen){
    (void)ctx;assert(pid==getpid()&&lease[0]==1&&gen==1);
    begins++;return 0;
}
static int complete(void *ctx,pid_t pid,const uint8_t lease[16],
                    uint64_t gen,uint64_t fence){
    (void)ctx;assert(pid==getpid()&&lease[0]==1&&gen==1&&fence==77);
    completions++;return 0;
}
int main(void){
    RDGPUGuestState state;
    rdgpu_guest_state_init(&state,getpid(),transport,NULL);
    rdgpu_guest_set_image_stride_transport(&state,stride,NULL);
    rdgpu_guest_set_fd_export_transport(&state,issue,NULL);
    rdgpu_guest_set_fd_frame_transport(&state,begin,complete,NULL);
    long image=rdgpu_guest_syscall(&state,RDGPU_CREATE_BGRA8,16,16,0,0,0);
    assert(image>0);
    assert(rdgpu_guest_syscall(&state,RDGPU_GUEST_FD_EXPORT,image,16,16,0,0)==123);
    assert(exports==1&&begins==0&&completions==0);
    assert(state.slots[0].carrierIssued && !state.slots[0].exported);
    assert(rdgpu_guest_syscall(&state,RDGPU_GUEST_FD_EXPORT,image,16,16,0,0)==-EPERM);
    assert(rdgpu_guest_syscall(&state,RDGPU_CLEAR,image,1,0,0,0)==77);
    assert(begins==1 && !state.slots[0].producerComplete);
    assert(rdgpu_guest_syscall(&state,RDGPU_WAIT,image,77,0,0,0)==0);
    assert(completions==1 && state.slots[0].producerComplete);
    assert(rdgpu_guest_syscall(&state,RDGPU_CLEAR,image,2,0,0,0)==77);
    assert(begins==2);
    puts("PASS guest pending FD survives first and second draw lifecycle");
    return 0;
}
