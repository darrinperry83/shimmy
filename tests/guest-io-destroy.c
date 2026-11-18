#include <stdint.h>
#include <stdio.h>

static long raw_io_setup(unsigned n, uint64_t *ctx) {
    register long x0 __asm__("x0") = n;
    register long x1 __asm__("x1") = (long)ctx;
    register long x8 __asm__("x8") = 0;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}

static long raw_io_destroy(uint64_t ctx) {
    register long x0 __asm__("x0") = (long)ctx;
    register long x8 __asm__("x8") = 1;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
    return x0;
}

int main(void) {
    uint64_t ctx = 0;
    if (raw_io_destroy(0) != -22) return 1;
    if (raw_io_destroy(0x12340000) != -22) return 2;
    if (raw_io_setup(2, &ctx) != 0 || !ctx) return 3;
    if (raw_io_destroy(ctx) != 0) return 4;
    if (raw_io_destroy(ctx) != -22) return 5;

    uint64_t many[32] = {0};
    for (int i = 0; i < 32; ++i)
        if (raw_io_setup(1, &many[i]) != 0 || !many[i]) return 6;
    uint64_t extra = 0;
    if (raw_io_setup(1, &extra) != -11 || extra) return 7;
    if (raw_io_destroy(many[15]) != 0) return 8;
    many[15] = 0;
    if (raw_io_setup(1, &extra) != 0 || !extra) return 9;
    for (int i = 0; i < 32; ++i)
        if (many[i] && raw_io_destroy(many[i]) != 0) return 10;
    if (raw_io_destroy(extra) != 0) return 11;
    puts("GUEST_IO_DESTROY_OK");
    return 0;
}
