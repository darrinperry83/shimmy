#include "../src/rdgpu_guest_syscall.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static RDGPUReply transport(void *unused, const RDGPURequest *q) {
    (void)unused;
    RDGPUReply r={.magic=RDGPU_WIRE_MAGIC,.version=RDGPU_WIRE_VERSION,
        .bytes=RDGPU_WIRE_BYTES,.request_id=q->request_id,.status=RDGPU_OK};
    if(q->op==RDGPU_CREATE_BGRA8){r.lease[0]=1;r.value1=1;}
    else memcpy(r.lease,q->lease,16);
    return r;
}

static int resolve(void *unused,pid_t pid,int fd,uint32_t width,
                   uint32_t height,int can_import_new,
                   RDGPUFDImportResult *out) {
    (void)unused;
    assert(pid==getpid());
    memset(out,0,sizeof *out);
    if(fd==101){out->lease[0]=1;out->generation=1;
                out->width=16;out->height=16;return 0;}
    if(fd==102){
        if(!can_import_new)return -ENOSPC;
        if(width!=32||height!=24)return -ESTALE;
        out->lease[0]=2;out->generation=5;
        out->width=32;out->height=24;return 0;
    }
    return -ESTALE;
}

int main(void) {
    RDGPUGuestState state;
    rdgpu_guest_state_init(&state,getpid(),transport,NULL);
    rdgpu_guest_set_fd_import_transport(&state,resolve,NULL);
    long local=rdgpu_guest_syscall(&state,RDGPU_CREATE_BGRA8,16,16,0,0,0);
    assert(local>0);
    assert(rdgpu_guest_syscall(&state,RDGPU_GUEST_FD_IMPORT,101,16,16,0,0)==local);
    assert(rdgpu_guest_syscall(&state,RDGPU_GUEST_FD_IMPORT,102,31,24,0,0)==-ESTALE);
    long foreign=rdgpu_guest_syscall(&state,RDGPU_GUEST_FD_IMPORT,102,32,24,0,0);
    assert(foreign>0 && foreign!=local);
    assert(state.slots[1].width==32 && state.slots[1].height==24);
    assert(state.slots[1].exported && state.slots[1].producerComplete);
    assert(rdgpu_guest_syscall(&state,RDGPU_GUEST_FD_IMPORT,102,32,24,0,0)==foreign);
    assert(rdgpu_guest_syscall(&state,RDGPU_GUEST_FD_IMPORT,102,32,23,0,0)==-ESTALE);
    assert(rdgpu_guest_syscall(&state,RDGPU_CLEAR,foreign,0,0,0,0)==-EBUSY);
    assert(rdgpu_guest_syscall(&state,RDGPU_RELEASE,foreign,0,0,0,0)==0);
    puts("PASS foreign broker lease receives one bounded read-only guest handle");
}
