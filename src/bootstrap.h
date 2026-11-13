// bootstrap.h — Linux aarch64 initial process stack (argc/argv/envp/auxv) + guest entry.
#ifndef RDRIFT_BOOTSTRAP_H
#define RDRIFT_BOOTSTRAP_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

struct prog_info {
  unsigned long base, entry, phdr;
  int           phnum, phent;
  unsigned long interp_base, interp_entry;   // 0 if the program is static
};

// Build the SysV aarch64 initial stack inside [stack, stack+stksz) and return the value the
// guest's SP must hold at entry (points at argc; 16-byte aligned).
// auxv must include AT_PHDR/PHENT/PHNUM/PAGESZ(16384)/BASE/ENTRY/RANDOM/EXECFN/UID/GID/CLKTCK
// and AT_SYSINFO_EHDR(33) = vdso_auxv_base() so libc uses the trap-free vDSO.
void* build_stack(void* stack, size_t stksz, int argc, char** argv, char** envp,
                  const struct prog_info* p);

// Interpreter entry when dynamic, else the program entry.
unsigned long entry_point(const struct prog_info* p);

// Set SP and branch to `entry` with a zeroed frame/link register. Never returns.
__attribute__((noreturn)) void enter_guest(unsigned long entry, void* sp);

// Resume a guest thread with a FULL register file. `r` is 33 words: x0..x30, then sp, then pc.
// This is what clone() needs — the child has to continue at the instruction after its `svc`
// with the parent's registers, its own stack and x0 == 0.
//
// x16 (IP0) is used to hold the branch target and is therefore NOT restored. That is ABI-safe:
// x16/x17 are the intra-procedure-call scratch registers, which the AAPCS64 allows a linker
// veneer to clobber at any call boundary, and no clone child has a live value in them (musl's
// __clone resumes into `cbz x0, 1f; ldp x1, x0, [sp], #16; blr x1`).
#define GUEST_REGS_WORDS 33
__attribute__((noreturn)) void enter_guest_regs(const unsigned long* r);

#ifdef __cplusplus
}
#endif
#endif
