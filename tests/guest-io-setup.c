#include <stdint.h>
#include <stdio.h>

static long raw_io_setup(unsigned n, uint64_t *ctx) {
    register long x0 __asm__("x0") = n;
    register long x1 __asm__("x1") = (long)ctx;
    register long x8 __asm__("x8") = 0;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}

struct ring_header {
    uint32_t id, nr, head, tail;
    uint32_t magic, compat_features, incompat_features, header_length;
};

int main(void) {
    uint64_t ctx = 0;
    if (raw_io_setup(1, (uint64_t *)1) != -14) return 1;
    if (raw_io_setup(0, &ctx) != -22) return 2;
    if (ctx != 0) return 3;
    if (raw_io_setup(1025, &ctx) != -11) return 4;
    if (raw_io_setup(2, &ctx) != 0 || !ctx) return 5;
    const volatile struct ring_header *ring = (const void *)(uintptr_t)ctx;
    if (ring->magic != 0xa10a10a1u || ring->nr < 3 || ring->head || ring->tail ||
        ring->header_length != sizeof(*ring) || !ring->compat_features) return 6;
    if (raw_io_setup(1, &ctx) != -22) return 7;
    puts("GUEST_IO_SETUP_OK");
    return 0;
}
