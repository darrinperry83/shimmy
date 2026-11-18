#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

/* Call the ARM64 Linux six-argument ABI directly to check x3 and ignored x4. */
static long raw_pwritev2(int fd, const struct iovec *iov, int count,
                         unsigned long low, unsigned long high, unsigned long flags) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)iov;
    register long x2 __asm__("x2") = count;
    register long x3 __asm__("x3") = (long)low;
    register long x4 __asm__("x4") = (long)high;
    register long x5 __asm__("x5") = (long)flags;
    register long x8 __asm__("x8") = 287;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3),
                     "r"(x4), "r"(x5), "r"(x8) : "memory");
    return x0;
}

int main(void) {
    int fd = open("/tmp/pwritev2-file", O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0) return 1;
    char a[] = "A", b[] = "B";
    struct iovec one = {a, 1}, two = {b, 1};
    if (raw_pwritev2(fd, &one, 1, 0, 0, 0) != 1) return 2;
    if (lseek(fd, 0, SEEK_SET) != 0) return 3;
    if (raw_pwritev2(fd, &two, 1, ~0UL, 17, 0) != 1) return 4;
    if (lseek(fd, 0, SEEK_CUR) != 1) return 5;

    /* Unsupported per-write sync must fail before changing data or offset. */
    if (raw_pwritev2(fd, &two, 1, 0, 0, 0x4) != -95) return 6;
    if (lseek(fd, 0, SEEK_CUR) != 1) return 7;
    char got[2] = {0};
    if (pread(fd, got, 1, 0) != 1 || got[0] != 'B') return 8;

    /* x3 carries the full 64-bit offset; x4 must not shift it again. */
    if (raw_pwritev2(fd, &one, 1, 0x100000000UL, 17, 0) != 1) return 9;
    if (lseek(fd, 0, SEEK_END) != (off_t)0x100000001ULL) return 10;
    if (lseek(fd, 0, SEEK_CUR) != (off_t)0x100000001ULL) return 11;
    puts("GUEST_PWRITEV2_OK");
    close(fd);
    return 0;
}
