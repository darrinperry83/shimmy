/* Read-only VM display diagnostic using the chooser's existing private API ABI. */
#include <IOKit/IOKitLib.h>
#include <IOSurface/IOSurface.h>
#include <dlfcn.h>
#include <stdio.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <string.h>
typedef void *Connection;
int main(void)
{
    setbuf(stdout, NULL);
    int hv=0; size_t n=sizeof(hv); struct utsname u;
    if (geteuid() || sysctlbyname("kern.hv_vmm_present",&hv,&n,NULL,0) || hv!=1 ||
        uname(&u) || !strstr(u.version,"ARM64_VMAPPLE")) return 1;
    void *lib=dlopen("/System/Library/PrivateFrameworks/IOMobileFramebuffer.framework/IOMobileFramebuffer",RTLD_NOW);
    if (!lib) { printf("IOMFB_DLOPEN %s\n",dlerror()); return 2; }
    kern_return_t (*main_display)(Connection *)=dlsym(lib,"IOMobileFramebufferGetMainDisplay");
    kern_return_t (*open_display)(io_service_t,task_port_t,unsigned,Connection *)=dlsym(lib,"IOMobileFramebufferOpen");
    kern_return_t (*get_surface)(Connection,int,IOSurfaceRef *)=dlsym(lib,"IOMobileFramebufferGetLayerDefaultSurface");
    if (!main_display || !open_display || !get_surface) return 3;
    Connection connection=NULL;
    printf("IOMFB_TRY_MAIN\n");
    kern_return_t kr=main_display(&connection);
    printf("IOMFB_MAIN kr=0x%x connection=%p\n",kr,connection);
    if (!connection) {
        io_service_t service=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching("AppleParavirtDisplay"));
        printf("IOMFB_SERVICE=%u\n",service);
        if (service) { kr=open_display(service,mach_task_self(),0,&connection); IOObjectRelease(service); }
        printf("IOMFB_OPEN kr=0x%x connection=%p\n",kr,connection);
    }
    if (!connection) return 4;
    IOSurfaceRef surface=NULL; kr=get_surface(connection,0,&surface);
    printf("IOMFB_SURFACE kr=0x%x surface=%p\n",kr,(void *)surface);
    if (!surface) return 5;
    printf("IOMFB_INFO id=%u width=%zu height=%zu stride=%zu bpe=%zu fmt=0x%x\n",IOSurfaceGetID(surface),IOSurfaceGetWidth(surface),IOSurfaceGetHeight(surface),IOSurfaceGetBytesPerRow(surface),IOSurfaceGetBytesPerElement(surface),IOSurfaceGetPixelFormat(surface));
    return 0;
}
