#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

/* Raw ARM64 Linux ABI: pkey_mprotect(addr, len, prot, pkey) is syscall 288. */
static long raw_pkey_mprotect(void *addr, size_t len, int prot, int pkey) {
    register long x0 __asm__("x0") = (long)addr;
    register long x1 __asm__("x1") = (long)len;
    register long x2 __asm__("x2") = (long)prot;
    register long x3 __asm__("x3") = (long)pkey;
    register long x8 __asm__("x8") = 288;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
    return x0;
}

int main(void) {
    enum { PAGE = 16384 };
    char *p = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return 1;
    p[0] = 'A';

    if (raw_pkey_mprotect(p, PAGE, PROT_READ, 0) != 0) return 2;
    if (p[0] != 'A') return 3;
    if (raw_pkey_mprotect(p, PAGE, PROT_READ | PROT_WRITE, -1) != 0) return 4;
    p[0] = 'B';
    if (p[0] != 'B') return 5;

    if (raw_pkey_mprotect(p, PAGE, PROT_READ, 2) != -22) return 6;
    if (raw_pkey_mprotect(p + 1, PAGE, PROT_READ, 0) != -22) return 7;
    if (raw_pkey_mprotect(p, PAGE, 0x80, 0) != -22) return 8;
    if (raw_pkey_mprotect(p, 0, PROT_READ, 2) != 0) return 9;
    if (raw_pkey_mprotect(p + 1, 0, PROT_READ, 0) != -22) return 10;
    p[0] = 'C'; // rejected requests must not alter the writable mapping
    if (p[0] != 'C') return 11;

    if (munmap(p, PAGE) != 0) return 12;
    puts("GUEST_PKEY_MPROTECT_OK");
    return 0;
}
