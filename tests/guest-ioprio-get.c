#include <stdio.h>

static long raw_ioprio_get(int which, int who) {
    register long x0 __asm__("x0") = which;
    register long x1 __asm__("x1") = who;
    register long x8 __asm__("x8") = 31;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}

static long raw_ioprio_set(int which, int who, int prio) {
    register long x0 __asm__("x0") = which;
    register long x1 __asm__("x1") = who;
    register long x2 __asm__("x2") = prio;
    register long x8 __asm__("x8") = 30;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return x0;
}

static long raw_getpid(void) {
    register long x0 __asm__("x0");
    register long x8 __asm__("x8") = 172;
    __asm__ volatile("svc #0" : "=r"(x0) : "r"(x8) : "memory");
    return x0;
}

int main(void) {
    long self = raw_getpid();
    if (self <= 0) return 1;
    if (raw_ioprio_get(1, 0) != 0) return 2;
    if (raw_ioprio_get(1, (int)self) != 0) return 3;
    if (raw_ioprio_set(1, 0, 0) != 0) return 4;
    if (raw_ioprio_get(1, 0) != 0) return 5;
    if (raw_ioprio_get(1, -1) != -3) return 6;
    if (raw_ioprio_get(1, 999999) != -3) return 7;
    if (raw_ioprio_get(0, 0) != -22) return 8;
    if (raw_ioprio_get(4, 0) != -22) return 9;
    if (raw_ioprio_get(2, 0) != -95) return 10;
    if (raw_ioprio_get(3, 0) != -95) return 11;
    puts("GUEST_IOPRIO_GET_OK");
    return 0;
}
