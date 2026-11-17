#include "../src/rdgpu_guest_syscall.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const uint8_t *allowed;
static int uploads;
static RDGPUReply transport(void *context,const RDGPURequest *q){
    (void)context;
    RDGPUReply r={.magic=RDGPU_WIRE_MAGIC,.version=RDGPU_WIRE_VERSION,
        .bytes=RDGPU_WIRE_BYTES,.request_id=q->request_id,.status=RDGPU_OK};
    if(q->op==RDGPU_CREATE_BGRA8){r.lease[0]=9;r.value1=1;}
    else memcpy(r.lease,q->lease,16);
    return r;
}
static int copyin(void *context,uint64_t address,void *snapshot,size_t bytes){
    (void)context;
    if(address!=(uintptr_t)allowed||bytes!=64)return -1;
    memcpy(snapshot,allowed,bytes);return 0;
}
static int upload(void *context,pid_t pid,const uint8_t lease[16],
                  uint64_t generation,uint32_t format,const void *snapshot,
                  size_t bytes){
    (void)context;
    assert(pid==getpid()&&lease[0]==9&&generation==1);
    assert(format==RDGPU_UPLOAD_RGBA8&&bytes==64);
    assert(!memcmp(snapshot,allowed,64));
    uploads++;return 0;
}
int main(void){
    RDGPUGuestState s;
    rdgpu_guest_state_init(&s,getpid(),transport,NULL);
    rdgpu_guest_set_image_upload_transport(&s,upload,copyin,NULL);
    long handle=rdgpu_guest_syscall(&s,RDGPU_CREATE_BGRA8,4,4,0,0,0);
    assert(handle>0);
    uint8_t pixels[64];
    for(unsigned i=0;i<64;i++)pixels[i]=(uint8_t)i;
    allowed=pixels;
    assert(rdgpu_guest_syscall(&s,RDGPU_GUEST_IMAGE_UPLOAD,handle,
        (uintptr_t)pixels,63,RDGPU_UPLOAD_RGBA8,0)==-EINVAL);
    assert(rdgpu_guest_syscall(&s,RDGPU_GUEST_IMAGE_UPLOAD,handle,
        (uintptr_t)pixels,64,99,0)==-EINVAL);
    assert(rdgpu_guest_syscall(&s,RDGPU_GUEST_IMAGE_UPLOAD,handle,
        (uintptr_t)pixels+1,64,RDGPU_UPLOAD_RGBA8,0)==-EFAULT);
    assert(uploads==0);
    assert(rdgpu_guest_syscall(&s,RDGPU_GUEST_IMAGE_UPLOAD,handle,
        (uintptr_t)pixels,64,RDGPU_UPLOAD_RGBA8,0)==0);
    assert(uploads==1&&s.slots[0].producerComplete);
    puts("PASS guest upload format, bounds, source admission and snapshot");
    return 0;
}
