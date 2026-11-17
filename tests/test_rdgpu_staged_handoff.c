#include "../src/rdgpu_surface_handoff.h"
#include "../src/rdgpu_virtual_carrier.h"
#include <IOSurface/IOSurface.h>
#include <CoreFoundation/CoreFoundation.h>
#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static IOSurfaceRef surface(void) {
    int32_t width = 64, height = 64, bytes = 4;
    uint32_t format = 0x42475241u;
    CFNumberRef w = CFNumberCreate(NULL, kCFNumberSInt32Type, &width);
    CFNumberRef h = CFNumberCreate(NULL, kCFNumberSInt32Type, &height);
    CFNumberRef b = CFNumberCreate(NULL, kCFNumberSInt32Type, &bytes);
    CFNumberRef p = CFNumberCreate(NULL, kCFNumberSInt32Type, &format);
    const void *keys[] = {kIOSurfaceWidth,kIOSurfaceHeight,
                          kIOSurfaceBytesPerElement,kIOSurfacePixelFormat};
    const void *values[] = {w,h,b,p};
    CFDictionaryRef d = CFDictionaryCreate(NULL, keys, values, 4,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    IOSurfaceRef s = IOSurfaceCreate(d);
    CFRelease(d); CFRelease(w); CFRelease(h); CFRelease(b); CFRelease(p);
    return s;
}
static void returned(void *context,const uint8_t lease[16],uint64_t generation) {
    (void)context;(void)lease;(void)generation;
}

int main(int argc,char **argv) {
    if(argc==3 && !strcmp(argv[1],"child")) {
        int inherited=atoi(argv[2]);uint64_t generation=0;
        assert(rdgpu_handoff_is_carrier_fd(inherited));
        assert(!rdgpu_handoff_is_pending_fd(inherited));
        assert(!rdgpu_handoff_frame_ready_fd(inherited,&generation));
        assert(rdgpu_handoff_resolve_fd(inherited)==NULL);
        return 0;
    }
    RDCarrier *carrier=rd_carrier_create(getpid(),1,returned,NULL);
    uint8_t lease[16]={1};
    RDCarrierFormat format={64,64,256,RDCARRIER_ARGB8888,0};
    RDCarrierCapability cap={1,1};
    int fd=-1;
    assert(carrier && rd_carrier_issue_pending(carrier,lease,1,format,cap,&fd));
    assert(fd>=0 && rdgpu_handoff_is_carrier_fd(fd));
    uint8_t bytes[32]={1};
    assert(pread(fd,bytes,sizeof bytes,0)==(ssize_t)sizeof bytes);
    for(size_t n=0;n<sizeof bytes;n++) assert(bytes[n]==0);
    int other=open("/dev/null",O_RDONLY);
    assert(other>=0);
    assert(!rdgpu_handoff_is_carrier_fd(other));
    assert(rdgpu_handoff_register_pending_fd(other)<0);
    assert(!rdgpu_handoff_is_pending_fd(other));
    assert(rdgpu_handoff_register_pending_fd(fd)==0);
    assert(rdgpu_handoff_register_pending_fd(fd)<0);
    int duplicate=dup(fd);
    assert(duplicate>=0 && rdgpu_handoff_is_pending_fd(duplicate));
    uint64_t generation=42;
    assert(!rdgpu_handoff_frame_ready_fd(fd,&generation) && generation==0);
    assert(rdgpu_handoff_resolve_fd(fd)==NULL);
    IOSurfaceRef image=surface();
    assert(image);
    assert(rdgpu_handoff_publish_completed_fd(other,image,1)<0);
    assert(rd_carrier_arm_fence(carrier,lease,1,101));
    assert(rd_carrier_complete(carrier,lease,1,101));
    assert(rdgpu_handoff_publish_completed_fd(fd,image,1)==0);
    assert(!rdgpu_handoff_is_pending_fd(fd));
    assert(rdgpu_handoff_frame_ready_fd(duplicate,&generation)==1 && generation==1);
    int flags=fcntl(fd,F_GETFD);assert(flags>=0);
    assert(fcntl(fd,F_SETFD,flags&~FD_CLOEXEC)==0);
    pid_t child=fork();assert(child>=0);
    if(child==0){char number[32];snprintf(number,sizeof number,"%d",fd);
        execl(argv[0],argv[0],"child",number,(char *)NULL);_exit(127);}
    int status=0;assert(waitpid(child,&status,0)==child);
    assert(WIFEXITED(status) && WEXITSTATUS(status)==0);
    IOSurfaceRef redeemed=rdgpu_handoff_resolve_fd(duplicate);
    assert(redeemed && IOSurfaceGetID(redeemed)==IOSurfaceGetID(image));
    CFRelease(redeemed);
    assert(rdgpu_handoff_begin_write_fd(fd)<0); /* consumer still owns frame */
    assert(rdgpu_handoff_consumer_release_fd(other,1)<0);
    assert(rdgpu_handoff_consumer_release_fd(fd,2)<0); /* stale generation */
    assert(rdgpu_handoff_consumer_release_fd(fd,1)==0);
    assert(rdgpu_handoff_resolve_fd(fd)==NULL);
    assert(rdgpu_handoff_begin_write_fd(fd)==0);
    assert(rdgpu_handoff_is_pending_fd(fd));
    assert(!rdgpu_handoff_frame_ready_fd(fd,&generation));
    assert(rdgpu_handoff_publish_completed_fd(fd,image,1)<0); /* replay */
    assert(rd_carrier_arm_fence(carrier,lease,1,102));
    assert(rd_carrier_complete(carrier,lease,1,102));
    assert(rdgpu_handoff_publish_completed_fd(fd,image,2)==0);
    assert(rdgpu_handoff_frame_ready_fd(fd,&generation) && generation==2);
    struct stat st; assert(!fstat(fd,&st));
    rdgpu_handoff_revoke(st.st_dev,st.st_ino);
    assert(!rdgpu_handoff_is_pending_fd(fd));
    assert(!rdgpu_handoff_frame_ready_fd(fd,&generation));
    assert(rdgpu_handoff_resolve_fd(fd)==NULL);
    CFRelease(image);close(other);close(duplicate);close(fd);
    rd_carrier_destroy(carrier);
    puts("PASS staged handoff pending, ready, release, replay and revoke");
    return 0;
}
