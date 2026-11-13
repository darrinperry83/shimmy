// fastcall.h — trap-free syscall fast path: rewrite guest `svc #0` -> `bl <thunk>`.
//
// The Mach-exception trap costs ~9.7us per syscall (cross-thread round trip). Calling a thunk
// in-process instead costs ~2ns. The thunk marshals x0..x5/x8 into the same dispatch the trap
// handler uses, then returns to the guest — no kernel, no thread hand-off.
//
// SAFETY (learned the hard way): `bl` CLOBBERS x30. musl leaf syscall wrappers look like
//     mov x8,#nr ; svc #0 ; b __syscall_ret
// with x30 LIVE (they return through it), so patching those to `bl` corrupts the return address
// and hangs (e.g. lseek spins forever). fastcall_patch_ex() must therefore run an x30-liveness
// analysis and SKIP any site where x30 is live, leaving it as `brk #0` (trap path). ~80% of
// sites are safely patchable; the rest stay on the trap.
//
// Note the TLS rewrites (msr/mrs tpidr_el0 -> brk) are INDEPENDENT of this and always trap:
// they must not be fast-pathed, because leaf TLS accessors like __errno_location keep x30 live.
#ifndef RDRIFT_FASTCALL_H
#define RDRIFT_FASTCALL_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

// Host-side dispatch the thunk calls: same routing/ABI as the trap handler
// (negative LINUX errno on failure).
typedef long (*fastcall_dispatch_fn)(unsigned long nr, unsigned long a0, unsigned long a1,
                                     unsigned long a2, unsigned long a3, unsigned long a4,
                                     unsigned long a5);
void  fastcall_set_dispatch(fastcall_dispatch_fn fn);

// Emit the thunk into executable guest memory (via gmem_reserve/gmem_map_code) and return its
// address, or 0 on failure. The thunk MUST:
//   - preserve the guest's x18 (Darwin's platform register; zero it for the host call, restore after)
//   - save/restore the full NEON/FP register file (the Linux syscall ABI preserves v0-v31)
//   - switch to a dedicated clean NATIVE stack for the host call (macOS libsystem crashes when
//     run on the guest's Linux stack), then switch back
unsigned long fastcall_build_thunk(void);

// Build-at-most-once + the RDFASTCALL opt-in. Use this, not the raw builder.
unsigned long fastcall_thunk_once(void);

// Give THIS guest thread its native stack. Call from every guest thread entry, before any
// guest instruction runs: the thunk reads the slot with no null check, by design.
int fastcall_thread_init(void);

// Patch one executable image IN PLACE at its FINAL address (after it is in JIT). For every
// `brk #0` (already-rewritten svc) whose site has x30 dead, replace it with `bl thunk`.
// Returns the number of sites patched; `out_skipped` (optional) receives the x30-live count.
// Must this brk#0 site stay on the trap path? (clone/exit/execve/wait4/waitid)
int   fastcall_denied_site(const void* code, long nwords, long idx);

int   fastcall_patch_ex(void* code, size_t len, unsigned long thunk, int* out_skipped);

// Same, for a staging buffer whose final RUN address is `va` (the ld.so DSO path, where
// every real library including libc.so.6 arrives).
int   fastcall_patch_va(void* code, size_t len, unsigned long va,
                        unsigned long thunk, int* out_skipped);

#ifdef __cplusplus
}
#endif
#endif
