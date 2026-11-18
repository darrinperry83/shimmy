#include <stdio.h>

static long raw_ioprio_set(int which, int who, int prio) {
    register long x0 __asm__("x0") = which;
    register long x1 __asm__("x1") = who;
    register long x2 __asm__("x2") = prio;
    register long x8 __asm__("x8") = 30;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return x0;
}

int main(void) {
    if (raw_ioprio_set(1, 0, 0) != 0) return 1;
    if (raw_ioprio_set(1, 0, 2 << 13 | 4) != -95) return 2;
    if (raw_ioprio_set(1, 0, 3 << 13) != -95) return 3;
    if (raw_ioprio_set(1, 0, 7 << 13) != -22) return 4;
    if (raw_ioprio_set(1, 0, 1) != -22) return 5;
    if (raw_ioprio_set(99, 0, 0) != -22) return 6;
    if (raw_ioprio_set(1, -1, 0) != -3) return 7;
    if (raw_ioprio_set(2, 0, 0) != -95) return 8;
    puts("GUEST_IOPRIO_SET_OK");
    return 0;
}
