#include "../third_party/rdgpu_broker.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static RDGPURequest request(uint16_t op,uint64_t id) {
    return (RDGPURequest){.magic=RDGPU_WIRE_MAGIC,
        .version=RDGPU_WIRE_VERSION,.op=op,.bytes=RDGPU_WIRE_BYTES,
        .request_id=id};
}
int main(void) {
    RDGPUSession *session=rdgpu_local_session_create();
    assert(session);
    RDGPURequest create=request(RDGPU_CREATE_BGRA8,1);
    create.arg0=3u | (2ull<<32);
    RDGPUReply r=rdgpu_dispatch(session,&create);
    assert(r.status==RDGPU_OK && r.value1);
    uint8_t rgba[24]={1,2,3,4, 5,6,7,8, 9,10,11,12,
                      13,14,15,16, 17,18,19,20, 21,22,23,24};
    assert(!rdgpu_test_export_surface(session,r.lease,r.value1));
    IOSurfaceRef sample=rdgpu_local_sample_surface(session,r.lease,r.value1);
    assert(sample && IOSurfaceLock(sample,kIOSurfaceLockReadOnly,NULL)==kIOReturnSuccess);
    const uint8_t *zero=IOSurfaceGetBaseAddress(sample);
    assert(zero && zero[0]==0 && zero[1]==0 && zero[2]==0 && zero[3]==0);
    IOSurfaceUnlock(sample,kIOSurfaceLockReadOnly,NULL);
    CFRelease(sample);
    assert(rdgpu_local_upload_image(session,r.lease,r.value1,
           RDGPU_UPLOAD_RGBA8,rgba,sizeof rgba)==0);
    sample=rdgpu_local_sample_surface(session,r.lease,r.value1);
    assert(sample);
    CFRelease(sample);
    IOSurfaceRef surface=rdgpu_test_export_surface(session,r.lease,r.value1);
    assert(surface);
    assert(IOSurfaceLock(surface,kIOSurfaceLockReadOnly,NULL)==kIOReturnSuccess);
    const uint8_t *base=IOSurfaceGetBaseAddress(surface);
    size_t stride=IOSurfaceGetBytesPerRow(surface);
    assert(base && stride>=12);
    assert(base[0]==3 && base[1]==2 && base[2]==1 && base[3]==4);
    assert(base[stride]==15 && base[stride+1]==14 &&
           base[stride+2]==13 && base[stride+3]==16);
    for(size_t i=12;i<stride;i++)assert(base[i]==0);
    IOSurfaceUnlock(surface,kIOSurfaceLockReadOnly,NULL);
    assert(rdgpu_local_upload_image(session,r.lease,r.value1,
           RDGPU_UPLOAD_RGBA8,rgba,sizeof rgba)==-EBUSY);
    assert(rdgpu_test_return_surface(session,r.lease,r.value1,surface));
    assert(rdgpu_local_upload_image(session,r.lease,r.value1,
           99,rgba,sizeof rgba)==-EINVAL);
    assert(rdgpu_local_upload_image(session,r.lease,r.value1,
           RDGPU_UPLOAD_BGRA8,rgba,sizeof rgba-1)==-EINVAL);
    assert(rdgpu_local_upload_image(session,r.lease,r.value1+1,
           RDGPU_UPLOAD_BGRA8,rgba,sizeof rgba)==-ESTALE);
    assert(rdgpu_local_upload_image(session,r.lease,r.value1,
           RDGPU_UPLOAD_BGRX8,rgba,sizeof rgba)==0);
    surface=rdgpu_test_export_surface(session,r.lease,r.value1);
    assert(surface && IOSurfaceLock(surface,kIOSurfaceLockReadOnly,NULL)==kIOReturnSuccess);
    base=IOSurfaceGetBaseAddress(surface);
    assert(base[0]==1 && base[1]==2 && base[2]==3 && base[3]==255);
    IOSurfaceUnlock(surface,kIOSurfaceLockReadOnly,NULL);
    assert(rdgpu_test_return_surface(session,r.lease,r.value1,surface));
    RDGPURequest clear=request(RDGPU_CLEAR,2);
    memcpy(clear.lease,r.lease,16);clear.arg1=r.value1;
    RDGPUReply pending=rdgpu_dispatch(session,&clear);
    assert(pending.status==RDGPU_OK && pending.value0);
    assert(!rdgpu_local_sample_surface(session,r.lease,r.value1));
    assert(rdgpu_local_upload_image(session,r.lease,r.value1,
           RDGPU_UPLOAD_BGRA8,rgba,sizeof rgba)==-EBUSY);
    RDGPURequest wait=request(RDGPU_WAIT,3);
    memcpy(wait.lease,r.lease,16);wait.arg0=pending.value0;wait.arg1=r.value1;
    int finished=0;
    for(unsigned i=0;i<1000;i++){
        RDGPUReply done=rdgpu_dispatch(session,&wait);
        if(done.status==RDGPU_OK){finished=1;break;}
        assert(done.status==RDGPU_PENDING);
        wait.request_id++;
        usleep(1000);
    }
    assert(finished);
    sample=rdgpu_local_sample_surface(session,r.lease,r.value1);
    assert(sample);
    CFRelease(sample);
    int final_upload=rdgpu_local_upload_image(session,r.lease,r.value1,
           RDGPU_UPLOAD_BGRA8,rgba,sizeof rgba);
    assert(final_upload==0);
    rdgpu_session_disconnect(session);
    puts("PASS existing broker image upload, format conversion, bounds and fence");
    return 0;
}
