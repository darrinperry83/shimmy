/* VM console output proof using the real AppleParavirtDisplay user client.
 * Private SwapSetLayer ABI: https://github.com/julioverne/screendump/blob/master/screendump/main.mm
 * Native host for a Linux child; the child executes through XNU's Linux ABI.
 */
#include <IOKit/IOKitLib.h>
#include <IOSurface/IOSurface.h>
#include <CoreGraphics/CGGeometry.h>
#include <dlfcn.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include "../../../src/chooser_font.h"
typedef void *Connection;
static void number(CFMutableDictionaryRef d, CFStringRef key, int64_t value)
{
    CFNumberRef n=CFNumberCreate(NULL,kCFNumberSInt64Type,&value);
    CFDictionarySetValue(d,key,n); CFRelease(n);
}
static size_t dimension(io_service_t service,CFStringRef key)
{
    CFTypeRef property=IORegistryEntryCreateCFProperty(service,key,NULL,0);
    int64_t value=0;
    if(property && CFGetTypeID(property)==CFNumberGetTypeID()) CFNumberGetValue(property,kCFNumberSInt64Type,&value);
    if(property) CFRelease(property);
    return value>0 && value<=8192 ? (size_t)value : 0;
}
static void text(uint32_t *pixels,size_t width,size_t height,size_t stride,const char *bytes)
{
    size_t x=24,y=24;
    for(;*bytes;bytes++) {
        unsigned char c=(unsigned char)*bytes;
        if(c=='\r') {x=24;continue;}
        if(c=='\n') {x=24;y+=32;continue;}
        if(x+16>width) {x=24;y+=32;}
        if(y+32>height) break;
        if(c>=CHOOSER_FONT_FIRST && c<=CHOOSER_FONT_LAST) {
            const unsigned char *glyph=chooser_font8x13[c-CHOOSER_FONT_FIRST];
            for(size_t row=0;row<13;row++) for(size_t col=0;col<8;col++)
                if(glyph[12-row] & (0x80>>col))
                    for(size_t dy=0;dy<2;dy++) for(size_t dx=0;dx<2;dx++)
                        pixels[(y+row*2+dy)*stride+x+col*2+dx]=0xffe8eef2;
        }
        x+=16;
    }
}
int main(int argc,char **argv)
{
    setbuf(stdout,NULL);
    int hv=0;size_t n=sizeof(hv);struct utsname u;
    if(argc<2 || geteuid() || sysctlbyname("kern.hv_vmm_present",&hv,&n,NULL,0) || hv!=1 ||
       uname(&u) || !strstr(u.version,"ARM64_VMAPPLE")) return 1;
    io_service_t service=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching("AppleParavirtDisplay"));
    if(!service) return 2;
    size_t width=dimension(service,CFSTR("DisplayWidth")),height=dimension(service,CFSTR("DisplayHeight"));
    if(!width || !height) return 3;
    void *lib=dlopen("/System/Library/PrivateFrameworks/IOMobileFramebuffer.framework/IOMobileFramebuffer",RTLD_NOW);
    if(!lib) return 4;
    kern_return_t (*open_display)(io_service_t,task_port_t,unsigned,Connection *)=dlsym(lib,"IOMobileFramebufferOpen");
    kern_return_t (*begin)(Connection,int *)=dlsym(lib,"IOMobileFramebufferSwapBegin");
    kern_return_t (*layer)(Connection,int,IOSurfaceRef,CGRect,CGRect,int)=dlsym(lib,"IOMobileFramebufferSwapSetLayer");
    kern_return_t (*end)(Connection)=dlsym(lib,"IOMobileFramebufferSwapEnd");
    if(!open_display || !begin || !layer || !end) return 5;
    Connection connection=NULL;kern_return_t kr=open_display(service,mach_task_self(),0,&connection);IOObjectRelease(service);
    printf("CONSOLE_DISPLAY_OPEN kr=0x%x width=%zu height=%zu\n",kr,width,height);
    if(kr || !connection) return 6;
    CFMutableDictionaryRef props=CFDictionaryCreateMutable(NULL,0,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    number(props,kIOSurfaceWidth,width);number(props,kIOSurfaceHeight,height);
    number(props,kIOSurfaceBytesPerElement,4);number(props,kIOSurfaceBytesPerRow,width*4);
    number(props,kIOSurfaceAllocSize,width*height*4);number(props,kIOSurfacePixelFormat,0x42475241);
    IOSurfaceRef surface=IOSurfaceCreate(props);CFRelease(props);
    if(!surface) return 7;
    char output[131072]="RDXNU - Linux console output proof\nLinux ELF on XNU; Apple display driver; no WindowServer\nKeyboard input is not implemented yet.\n\n";
    size_t used=strlen(output);int pipes[2];if(pipe(pipes)) return 8;
    pid_t child=fork();if(child<0) return 9;
    if(!child) {close(pipes[0]);dup2(pipes[1],1);dup2(pipes[1],2);close(pipes[1]);execv(argv[1],argv+1);_exit(127);}
    close(pipes[1]);ssize_t amount;
    while((amount=read(pipes[0],output+used,sizeof(output)-used-1))>0) {
        used+=(size_t)amount;if(used==sizeof(output)-1) break;
    }
    close(pipes[0]);output[used]=0;int status=0;waitpid(child,&status,0);
    printf("CONSOLE_LINUX_CHILD status=%d bytes=%zu\n",status,used);
    kr=IOSurfaceLock(surface,0,NULL);if(kr) return 10;
    uint32_t *pixels=IOSurfaceGetBaseAddress(surface);size_t stride=IOSurfaceGetBytesPerRow(surface)/4;
    if(!pixels || IOSurfaceGetBytesPerElement(surface)!=4) return 11;
    for(size_t y=0;y<height;y++) for(size_t x=0;x<width;x++) pixels[y*stride+x]=0xff101820;
    text(pixels,width,height,stride,output);IOSurfaceUnlock(surface,0,NULL);
    int swap=0;kr=begin(connection,&swap);printf("CONSOLE_SWAP_BEGIN kr=0x%x id=%d\n",kr,swap);if(kr) return 12;
    CGRect bounds=CGRectMake(0,0,width,height);
    kr=layer(connection,0,surface,bounds,bounds,0);printf("CONSOLE_SWAP_LAYER kr=0x%x surface=%u\n",kr,IOSurfaceGetID(surface));if(kr) return 13;
    kr=end(connection);printf("CONSOLE_SWAP_END kr=0x%x\n",kr);if(kr) return 14;
    fflush(stdout);sleep(60);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 15;
}
