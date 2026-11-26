/* VM-only IOFramebuffer diagnostic. No WindowServer or CoreGraphics connection. */
#include <IOKit/IOKitLib.h>
#include <IOKit/graphics/IOGraphicsLib.h>
#include <IOKit/graphics/IOGraphicsTypes.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv)
{
    int hv = 0; size_t size = sizeof(hv); struct utsname u;
    if (geteuid() || sysctlbyname("kern.hv_vmm_present", &hv, &size, NULL, 0) ||
        hv != 1 || uname(&u) || !strstr(u.version, "ARM64_VMAPPLE")) return 1;
    int activate = argc == 2 && !strcmp(argv[1], "--activate");
    io_iterator_t iterator = 0;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault,
        IOServiceMatching("IOFramebuffer"), &iterator);
    printf("FRAMEBUFFER_MATCH kr=0x%x\n", kr);
    if (kr) return 2;
    io_service_t service; unsigned count = 0;
    while ((service = IOIteratorNext(iterator))) {
        ++count; io_name_t name = {0}, class_name = {0};
        IORegistryEntryGetName(service, name); IOObjectGetClass(service, class_name);
        printf("FRAMEBUFFER name=%s class=%s\n", name, class_name);
        io_connect_t connection = 0;
        kr = IOFramebufferOpen(service, mach_task_self(), kIOFBServerConnectType, &connection);
        printf("FRAMEBUFFER_OPEN kr=0x%x connection=%u\n", kr, connection);
        if (!kr) {
            uint64_t mode[2] = {0}; uint32_t count_out = 2;
            kr = IOConnectCallScalarMethod(connection, 2, NULL, 0, mode, &count_out);
            printf("FRAMEBUFFER_MODE kr=0x%x mode=%llu depth=%llu\n", kr, mode[0], mode[1]);
            if (!kr && activate) {
                kr = IOConnectCallScalarMethod(connection, 4, mode, 2, NULL, NULL);
                printf("FRAMEBUFFER_ACTIVATE kr=0x%x\n", kr);
            }
            IOServiceClose(connection);
        }
        IOObjectRelease(service);
    }
    IOObjectRelease(iterator);
    printf("FRAMEBUFFER_COUNT=%u\n", count);
    return 0;
}
