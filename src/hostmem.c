// hostmem.c — route the Linux side's memcpy/memmove/memset to macOS libSystem's.
// There is no VM: the two sides share one address space, so this is a direct call,
// not a copy across a boundary. b7icd already calls host MoltenVK the same way.
#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

extern long syscall(long, ...);
#define HOSTCALL_SYSCALL 0x2001
enum { HC_DLOPEN = 1, HC_DLSYM = 2 };
struct hostcall_req { uint32_t op; uint32_t nargs; uint64_t a[8]; uint64_t result; };
static int hc(struct hostcall_req* r) { return (int)syscall(HOSTCALL_SYSCALL, r); }

static uint64_t h_dlopen(const char* p) {
    struct hostcall_req r; __builtin_memset(&r, 0, sizeof r);
    r.op = HC_DLOPEN; r.a[0] = (uint64_t)(uintptr_t)p;
    return hc(&r) < 0 ? 0 : r.result;
}
static uint64_t h_dlsym(uint64_t h, const char* n) {
    struct hostcall_req r; __builtin_memset(&r, 0, sizeof r);
    r.op = HC_DLSYM; r.a[0] = h; r.a[1] = (uint64_t)(uintptr_t)n;
    return hc(&r) < 0 ? 0 : r.result;
}

static void* (*hm_cpy)(void*, const void*, size_t);
static void* (*hm_move)(void*, const void*, size_t);
static void* (*hm_set)(void*, int, size_t);

__attribute__((constructor)) static void init(void) {
    uint64_t h = h_dlopen("/usr/lib/libSystem.B.dylib");
    if (!h) return;
    hm_cpy  = (void*)(uintptr_t)h_dlsym(h, "memcpy");
    hm_move = (void*)(uintptr_t)h_dlsym(h, "memmove");
    hm_set  = (void*)(uintptr_t)h_dlsym(h, "memset");
    // POSITIVE CONTROL: without this, a preload that silently failed to resolve
    // looks exactly like a preload that worked and changed nothing.
    if (getenv("RDHOSTMEM_VERBOSE"))
        fprintf(stderr, "[hostmem] libSystem=%llx memcpy=%p memmove=%p memset=%p\n",
                (unsigned long long)h, (void*)hm_cpy, (void*)hm_move, (void*)hm_set);
}

// Byte loops are the fallback for the window before the constructor runs -- the
// dynamic loader itself copies memory, and calling through a null pointer there
// would fault long before main().
void* memcpy(void* d, const void* s, size_t n) {
    if (hm_cpy) return hm_cpy(d, s, n);
    unsigned char* a = d; const unsigned char* b = s;
    for (size_t i = 0; i < n; i++) a[i] = b[i];
    return d;
}
void* memmove(void* d, const void* s, size_t n) {
    if (hm_move) return hm_move(d, s, n);
    unsigned char* a = d; const unsigned char* b = s;
    if (a < b) { for (size_t i = 0; i < n; i++) a[i] = b[i]; }
    else       { for (size_t i = n; i-- > 0; )  a[i] = b[i]; }
    return d;
}
void* memset(void* d, int c, size_t n) {
    if (hm_set) return hm_set(d, c, n);
    unsigned char* a = d;
    for (size_t i = 0; i < n; i++) a[i] = (unsigned char)c;
    return d;
}
