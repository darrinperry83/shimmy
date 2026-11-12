// rd_mac_probe.c — standalone validation helper for the mac-app IOSurface path.
// Plays the shim's importer side: registers the handoff receive channel, resolves
// the carrier fd back to the published IOSurface, then reads REAL pixels out of it
// (IOSurfaceLock + IOSurfaceGetBaseAddress, the same CPU readback the shim uses)
// at a few sample points, twice (to prove liveness), and dumps a PPM.
//
// Usage:
//   RDSOCKETGROUP=<grp> rd_mac_probe <carrier-file> <out.ppm>
// (run while rd_mac_app is publishing into the same carrier + socket group)
#include "rdgpu_surface_handoff.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurface.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int sample_rgb(IOSurfaceRef s, size_t x, size_t y, uint8_t rgb[3]) {
    rgb[0] = rgb[1] = rgb[2] = 0;
    if (!s) return -1;
    if (IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL) != kIOReturnSuccess) return -1;
    const uint8_t *base = IOSurfaceGetBaseAddress(s);
    const size_t pitch = IOSurfaceGetBytesPerRow(s);
    const size_t w = IOSurfaceGetWidth(s), h = IOSurfaceGetHeight(s);
    if (base && x < w && y < h) {
        const uint8_t *p = base + y * pitch + x * 4; // BGRA
        rgb[0] = p[2]; rgb[1] = p[1]; rgb[2] = p[0];
    }
    IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
    return 0;
}

static int dump_ppm(IOSurfaceRef s, const char *path) {
    if (!s || !path) return -1;
    if (IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL) != kIOReturnSuccess) return -1;
    const uint8_t *base = IOSurfaceGetBaseAddress(s);
    const size_t pitch = IOSurfaceGetBytesPerRow(s);
    const size_t w = IOSurfaceGetWidth(s), h = IOSurfaceGetHeight(s);
    FILE *f = fopen(path, "wb");
    if (f && base) {
        fprintf(f, "P6\n%zu %zu\n255\n", w, h);
        for (size_t y = 0; y < h; y++) {
            const uint8_t *row = base + y * pitch;
            for (size_t x = 0; x < w; x++) {
                const uint8_t *p = row + x * 4;
                const uint8_t rgb[3] = { p[2], p[1], p[0] };
                fwrite(rgb, 1, 3, f);
            }
        }
        fclose(f);
        fprintf(stderr, "[rd-mac-probe] dumped %s (%zux%zu)\n", path, w, h);
    } else if (f) fclose(f);
    IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: rd_mac_probe <carrier> <out.ppm>\n"); return 2; }
    rdgpu_handoff_import_init();

    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) fd = open(argv[1], O_CREAT | O_RDWR, 0600);
    if (fd < 0) { perror("carrier"); return 1; }
    struct stat st;
    if (fstat(fd, &st)) { perror("fstat"); return 1; }

    IOSurfaceRef s = NULL;
    for (int i = 0; i < 400 && !s; i++) {
        s = rdgpu_handoff_resolve_fd(fd);
        if (!s) usleep(5000);
    }
    if (!s) { fprintf(stderr, "[rd-mac-probe] resolve failed (no published surface)\n"); return 1; }
    fprintf(stderr, "[rd-mac-probe] resolved id=%u %zux%zu fmt=0x%08x stride=%zu\n",
            IOSurfaceGetID(s), IOSurfaceGetWidth(s), IOSurfaceGetHeight(s),
            IOSurfaceGetPixelFormat(s), IOSurfaceGetBytesPerRow(s));

    size_t w = IOSurfaceGetWidth(s), h = IOSurfaceGetHeight(s);
    uint8_t a[3], b[3], c[3], a2[3], b2[3], c2[3];
    // sample: top-left background, moving-bar sweep line, frame-dot band
    sample_rgb(s, w / 8, h / 8, a);
    sample_rgb(s, w / 2, h / 2, b);
    sample_rgb(s, w / 2, (size_t)(h * 0.95), c);
    usleep(400000);
    sample_rgb(s, w / 8, h / 8, a2);
    sample_rgb(s, w / 2, h / 2, b2);
    sample_rgb(s, w / 2, (size_t)(h * 0.95), c2);

    int animated = (a[0]!=a2[0] || a[1]!=a2[1] || a[2]!=a2[2] ||
                    b[0]!=b2[0] || b[1]!=b2[1] || b[2]!=b2[2] ||
                    c[0]!=c2[0] || c[1]!=c2[1] || c[2]!=c2[2]);
    int nonblack = (a[0]+a[1]+a[2] > 30) || (b[0]+b[1]+b[2] > 30) || (c[0]+c[1]+c[2] > 30);
    fprintf(stderr, "[rd-mac-probe] sample0 top=%u,%u,%u mid=%u,%u,%u dots=%u,%u,%u\n",
            a[0],a[1],a[2], b[0],b[1],b[2], c[0],c[1],c[2]);
    fprintf(stderr, "[rd-mac-probe] sample1 top=%u,%u,%u mid=%u,%u,%u dots=%u,%u,%u\n",
            a2[0],a2[1],a2[2], b2[0],b2[1],b2[2], c2[0],c2[1],c2[2]);
    fprintf(stderr, "[rd-mac-probe] animated=%d nonblack=%d\n", animated, nonblack);

    dump_ppm(s, argv[2]);
    CFRelease(s);
    close(fd);
    fprintf(stderr, "RD_MAC_PROBE_RESULT animated=%d nonblack=%d\n", animated, nonblack);
    return (animated && nonblack) ? 0 : 1;
}
