/* test_handoff.c — two-process proof of the cross-process dma-buf -> IOSurface
 * mach-port handoff.
 *
 *   publish mode  (plays shim_cli / rdgpu_mock_gate.c mock_fd_export):
 *     create an IOSurface, publish it keyed by the carrier file's fstat
 *     (st_dev, st_ino), and write the surface id to <idfile>.
 *
 *   resolve mode  (plays shim_gui / drmasahi.c PRIME_FD_TO_HANDLE):
 *     register the cross-process receive channel (rdgpu_handoff_import_init,
 *     what wlsrv_start() does), wait for <idfile>, then resolve the same
 *     carrier fd and succeed only when IOSurfaceGetID() matches.
 *
 * Both invocations inherit the same $RDSOCKETGROUP, so they derive the same
 * per-user bootstrap name and rendezvous without a parent-created channel.
 *
 *   ./test_handoff publish <carrier-file> <idfile>
 *   ./test_handoff resolve <carrier-file> <idfile>
 */
#include "rdgpu_surface_handoff.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurface.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static IOSurfaceRef make_surface(void) {
    int32_t w = 64, h = 64, bpe = 4, fmt = 0x42475241u; /* BGRA, as the mock uses */
    CFNumberRef nw = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &w);
    CFNumberRef nh = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &h);
    CFNumberRef nb = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &bpe);
    CFNumberRef nf = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &fmt);
    const void *keys[] = { kIOSurfaceWidth, kIOSurfaceHeight,
                           kIOSurfaceBytesPerElement, kIOSurfacePixelFormat };
    const void *vals[] = { nw, nh, nb, nf };
    CFDictionaryRef d = CFDictionaryCreate(kCFAllocatorDefault, keys, vals, 4,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    IOSurfaceRef s = IOSurfaceCreate(d);
    CFRelease(nw); CFRelease(nh); CFRelease(nb); CFRelease(nf); CFRelease(d);
    return s;
}

static int open_carrier(const char *path, struct stat *st) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) fd = open(path, O_CREAT | O_RDWR, 0600);
    if (fd < 0) { perror("carrier"); return -1; }
    if (fstat(fd, st)) { perror("fstat"); close(fd); return -1; }
    return fd;
}

static int do_publish(const char *carrier, const char *idfile) {
    struct stat st;
    int fd = open_carrier(carrier, &st);
    if (fd < 0) return 1;
    IOSurfaceRef s = make_surface();
    if (!s) { fprintf(stderr, "PUBLISH no surface\n"); return 1; }
    uint32_t id = IOSurfaceGetID(s);
    int rc = rdgpu_handoff_publish(s, st.st_dev, st.st_ino);
    FILE *f = fopen(idfile, "w");
    if (f) { fprintf(f, "%u\n", id); fclose(f); }
    fprintf(stderr, "PUBLISH id=%u dev=%d ino=%llu rc=%d\n",
            id, (int)st.st_dev, (unsigned long long)st.st_ino, rc);
    fflush(stderr);
    for (int i = 0; i < 40; i++) usleep(100000);   /* keep the surface alive 4s */
    rdgpu_handoff_revoke(st.st_dev, st.st_ino);
    CFRelease(s);
    close(fd);
    return rc == 0 ? 0 : 1;
}

static int do_resolve(const char *carrier, const char *idfile) {
    /* Mirror wlsrv_start(): register before the exporter publishes. */
    rdgpu_handoff_import_init();

    uint32_t expected = 0;
    for (int i = 0; i < 100 && !expected; i++) {
        FILE *f = fopen(idfile, "r");
        if (f) {
            if (fscanf(f, "%u", &expected) != 1) expected = 0;
            fclose(f);
        }
        if (!expected) usleep(50000);   /* 50ms * 100 = 5s bound */
    }
    if (!expected) { fprintf(stderr, "RESOLVE no published id\n"); return 1; }

    struct stat st;
    int fd = open_carrier(carrier, &st);
    if (fd < 0) return 1;
    IOSurfaceRef s = rdgpu_handoff_resolve_fd(fd);
    uint32_t got = s ? IOSurfaceGetID(s) : 0;
    int match = (s != NULL && got == expected);
    fprintf(stderr, "RESOLVE id=%u expected=%u match=%d\n", got, expected, match);
    fflush(stderr);
    if (s) CFRelease(s);
    close(fd);
    return match ? 0 : 1;
}

static int do_inproc(const char *carrier) {
    /* Same-process publish->resolve: exercises the process-local table, the
     * path that must keep working alongside the new cross-process transport. */
    struct stat st;
    int fd = open_carrier(carrier, &st);
    if (fd < 0) return 1;
    IOSurfaceRef s = make_surface();
    if (!s) { fprintf(stderr, "INPROC no surface\n"); return 1; }
    uint32_t id = IOSurfaceGetID(s);
    int rc = rdgpu_handoff_publish(s, st.st_dev, st.st_ino);
    IOSurfaceRef got = rdgpu_handoff_resolve_fd(fd);
    uint32_t got_id = got ? IOSurfaceGetID(got) : 0;
    int match = (got != NULL && got_id == id);
    fprintf(stderr, "INPROC id=%u resolved=%u match=%d rc=%d\n", id, got_id, match, rc);
    if (got) CFRelease(got);
    rdgpu_handoff_revoke(st.st_dev, st.st_ino);
    CFRelease(s);
    close(fd);
    return (rc == 0 && match) ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "inproc")) return do_inproc(argv[2]);
    if (argc != 4) {
        fprintf(stderr, "usage: %s <publish|resolve|inproc> <carrier-file> [<idfile>]\n", argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "publish")) return do_publish(argv[2], argv[3]);
    if (!strcmp(argv[1], "resolve")) return do_resolve(argv[2], argv[3]);
    fprintf(stderr, "unknown mode %s\n", argv[1]);
    return 2;
}
