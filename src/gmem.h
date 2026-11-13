// gmem.h — guest address-space / W^X JIT memory manager.
//
// Apple Silicon forbids RWX. Guest EXECUTABLE memory must be MAP_JIT (whose writability is
// toggled per-thread via pthread_jit_write_protect_np); guest DATA is plain RW. Callers never
// write JIT memory directly — they stage through gmem_map_code().
//
// THREADING: called from the Mach exception handler with the guest thread stopped, and from the
// loader thread before the guest runs. The exception-server thread must be write-protect(0)-capable;
// the guest thread stays write-protect(1) so JIT text is executable there.
#ifndef RDRIFT_GMEM_H
#define RDRIFT_GMEM_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

void  gmem_init(void);

// Reserve a tracked MAP_JIT region (kernel-placed; MAP_JIT cannot combine with MAP_FIXED).
void* gmem_reserve(size_t len);

// Copy `len` bytes of `bytes` into JIT memory at `addr` (must lie inside a tracked region),
// toggling this thread's write protection and invalidating icache. Returns 0 on success.
int   gmem_map_code(void* addr, const void* bytes, size_t len);

// Linux mmap/mprotect/munmap for the guest. `flags` uses LINUX values
// (MAP_SHARED=0x01, MAP_PRIVATE=0x02, MAP_FIXED=0x10, MAP_ANONYMOUS=0x20).
// Returns the address, or a negative errno.
long  gmem_mmap(unsigned long addr, size_t len, int prot, int flags, int fd, long off);
long  gmem_mprotect(unsigned long addr, size_t len, int prot);
long  gmem_munmap(unsigned long addr, size_t len);
int gmem_guest_register_main_stack(void *address,size_t length);
int gmem_guest_export_copyout(void *unused,uint64_t guestAddress,
                              const void *bytes,size_t length);
void gmem_guest_ranges_fork_child(void);

// Bulk-load bracket: batches the per-thread write-protect toggles and coalesces icache
// invalidation across many gmem_map_code() calls (big win when a DSO has many segments).
// Reentrant/refcounted. gmem_map_code() outside a bracket behaves exactly as before.
void  gmem_begin_load(void);
void  gmem_end_load(void);

// Rewrite guest instructions in an executable image so the shim can intercept them.
// MUST be applied to every executable segment of every image (main, interpreter, and every
// runtime-dlopen'd DSO), operating on the PLAIN staging buffer before it is copied into JIT:
//   svc  #0            (0xD4000001)      -> brk #0            : Linux syscall
//   msr  tpidr_el0, xN (0xD51BD040|rt)   -> brk #(0x100+rt)   : guest WRITES its TLS pointer
//   mrs  xN, tpidr_el0 (0xD53BD040|rt)   -> brk #(0x200+rt)   : guest READS its TLS pointer
// The TLS rewrites are mandatory: macOS owns the real TPIDR_EL0 and clobbers it on every
// context switch, so the guest's thread pointer CANNOT live in the hardware register.
void  gmem_rewrite_exec_insns(unsigned char* buf, size_t len);

// ---------------------------------------------------------------------------------------------
// ADDED (the declarations above are unchanged and keep working exactly as before).
//
// macOS OWNS x18 as well as TPIDR_EL0: XNU writes 0 into it on every exception return to EL0,
// asynchronously, ~1500 times a second. Linux aarch64 code uses x18 as an ordinary caller-saved
// temporary and keeps values live in it for dozens of instructions, so the guest cannot have it
// either. gmem_rewrite_exec_insns_va() does everything gmem_rewrite_exec_insns() does AND moves
// the guest's x18 out of the register file into gmem_guest_x18, by replacing each instruction
// that names x18 with a branch to a veneer that re-executes it on a borrowed register.
//
// `va` is the guest virtual address that buf[0] will occupy (the veneers must branch back to it);
// va == 0 disables the x18 pass, making this identical to gmem_rewrite_exec_insns().
//
// It MUST be given a range that contains only real instructions -- an executable section, not a
// whole executable segment, whose co-located .rodata would otherwise be rewritten as if it were
// code. elfload.c derives those ranges from the ELF section headers.
void  gmem_rewrite_exec_insns_va(unsigned char* buf, size_t len, unsigned long va);

// The guest's x18, virtualised into memory. Veneers load and store it directly.
extern unsigned long gmem_guest_x18;

// This thread's x18 storage: the per-thread TSD slot the veneers address, or the global when
// per-thread veneers are off. BOTH MUST BE CALLED ON THE THREAD THEY REFER TO — they read
// TPIDRRO_EL0, which is per-thread, so calling them from the exception-server thread would
// name the wrong slot. gmem_x18_seed() exists because a new pthread's TSD slots are zero and
// nothing else ever writes this one; a new guest thread's veneers would otherwise see x18 == 0.
unsigned long* gmem_x18_slot(void);
void           gmem_x18_seed(unsigned long v);

// The guest's THREAD POINTER (tpidr_el0), virtualised into memory, and this thread's home for it.
// Same contract as the x18 pair above in every respect — per-thread TSD slot when TLS veneering is
// on, the global otherwise, and both MUST be called on the thread they refer to.
//
// The reason this exists is that `mrs xN, tpidr_el0` is now VENEERED rather than trapped, and a
// veneer reads this thread's TSD slot inline. The trap handler still services the (rare) WRITE and
// any read a veneer could not be planted for, so it has to reach the SAME WORD: it does that
// through gthread::tpidr_slot, which the guest thread records here while running on itself.
// gmem_tls_slot() returns NULL when there is no per-thread storage, meaning "no inline home
// exists; the trap handler's per-thread gthread::tpidr is the only home". A caller MUST propagate
// that NULL and must NOT substitute a global of its own: doing so gives every guest thread one
// thread pointer, which is silent cross-thread corruption (glibc errno/locale/malloc-arena).
unsigned long* gmem_tls_slot(void);
void           gmem_tls_seed(unsigned long v);

// Choose where x18 lives (TSD slot vs the global) BEFORE anything records a slot address.
// Idempotent, and called anyway by the first image that is neutralized; shim.c calls it early so
// that no thread can bind a slot address before the choice is made. Binding first would give that
// thread &gmem_guest_x18 while later-emitted veneers use the TSD slot — the exact two-homes split
// this whole mechanism exists to prevent.
void           gmem_x18_tsd_init(void);

// Which 5-bit fields of an A64 instruction are general-purpose register operands (a mask of
// bit 0 => bits 4:0, bit 1 => bits 9:5, bit 2 => bits 14:10, bit 3 => bits 20:16), or -1 when the
// encoding is not one we are sure about. Exposed so tools/x18check.c can validate it against a
// real disassembler over every word of every rootfs binary.
int   gmem_x18_gpr_fields(unsigned int insn);

// Classify one instruction as an x18 site. Returns GMEM_X18_NONE when it does not name x18, one of
// the positive kinds when a veneer can be built for it, or GMEM_X18_UNSUP when the encoding cannot
// be rewritten safely (which makes the whole enclosing range fall back to the old behaviour).
// `*fields` receives, for GMEM_X18_PLAIN, the mask of operand fields that hold 18.
enum { GMEM_X18_NONE = 0, GMEM_X18_PLAIN, GMEM_X18_ADR, GMEM_X18_TBZ,
       GMEM_X18_CBZ,  GMEM_X18_LDRLIT, GMEM_X18_UNSUP };
int   gmem_x18_classify(unsigned int insn, unsigned* fields);

// Does this one word get special treatment in pass 0 of x18_neutralize_ranges?
// 0 = no, 1 = misaligned-SP prologue, 2 = register branch through x18. Exposed so tools/x18check.c
// can report the veneer / trap / racy split per image instead of only classifying words.
//
// The two are NOT the same escape hatch, and a caller that treats them alike is reproducing the
// bug that put 17,666 sites on the trap path for 17 words:
//   1 (misaligned SP)     -> WHOLE image trapped. A correctness requirement: any veneer in that
//                            image can fault on its own spill, wherever it is.
//   2 (register branch)    -> that ONE WORD trapped, the rest of the image veneered, unless
//                            gmem_x18_brwhole() says otherwise.
int   gmem_x18_pass0(unsigned int insn);

// Is `br`/`ret x18` still handled WHOLE-image (the pre-per-site policy)? RDX18_BRWHOLE=1, the A/B
// control for per-site branch trapping. Exported so the tool and the rewriter read ONE flag.
int   gmem_x18_brwhole(void);

// Is x18 stored per-thread (Darwin TSD slot, what the veneers address inline) or in the one
// process-global word? Callable from any thread; unlike gmem_x18_slot() it reads no register.
int   gmem_x18_tsd_on(void);

// Diagnostics: map a PC inside a veneer back to the guest instruction it stands in for.
// A veneer executes at an address in the JIT arena, so a fault inside one otherwise reports a
// PC that belongs to no image. Returns 1 and fills *site/*insn on a hit, 0 otherwise.
int   gmem_veneer_lookup(unsigned long pc, unsigned long* site, unsigned int* insn);

// Which loaded image contains `pc`? Fills *base with the image's load address and returns its
// path, or NULL when the address belongs to no tracked image. Fault reports are close to useless
// without this: a runtime DSO lands at an arbitrary address, so "interp_base + 0x1dad48c8" names
// nothing, and identifying the library by hand-matching instruction bytes is slow.
const char* gmem_image_for(unsigned long pc, unsigned long* base);

// --- trap-mode x18 sites ------------------------------------------------------------------
// Some images cannot be veneered at all: a veneer spills scratch registers through SP, and
// AArch64 faults on ANY SP-based access when SP is not 16-byte aligned. Hand-written assembly
// does run with a misaligned SP (pixman's NEON prologue does `sub sp, sp, #0xe8`), so for those
// images each x18 instruction becomes `brk #0x300` and is serviced here instead, where the
// handler has the whole register file and needs no scratch of its own.
//
// Both take the guest's x[0..30] array and update it in place. Return 1 if `pc` was such a site
// and *next holds the PC to resume at, 0 if it was not ours.
// A synthetic CTR_EL0. Darwin SIGILLs on `mrs xN, ctr_el0`, so the shim answers the read itself.
// 0x8444c004 declares 64-byte I- and D-cache lines; Apple's real line is 128 bytes, and reporting
// a SMALLER line is the safe direction — __clear_cache then steps in 64-byte increments and
// covers every real line twice. Claiming a larger line would skip lines and corrupt the icache.
#define GMEM_CTR_EL0_SYNTHETIC 0x8444c004UL

#define GMEM_BRK_X18_ENTER 0x300
#define GMEM_BRK_X18_LEAVE 0x301
// `x18` points at the CALLING GUEST THREAD's x18 storage. Once there is more than one guest
// thread the single global cannot be used — two threads running x18 code would clobber each
// other's value — so the handler supplies per-thread storage instead.
// `saved` MUST point at storage private to the trapping guest thread -- see the note in
// struct x18_trap. A shared slot silently swaps scratch registers between threads.
void  gmem_x18_trap_top(int topn);   // RDTRAPSTAT: hottest trapped sites

// FAULT-PATH DIAGNOSTIC. Report the trap records for the guest words AROUND the site that `pc`
// belongs to (pc may be the site itself, or a stub address), each with its live hit count and the
// word CURRENTLY IN GUEST MEMORY at that site.
//
// This exists to split one specific question that no other instrument can answer. When a trapped
// x18 READ faults on a shadow value that a straight-line x18 WRITE a few instructions earlier
// should have produced, exactly three things can be true, and the three are distinguished here:
//   * the neighbour has NO RECORD, and the guest word is not a brk  -> that write was never
//     rewritten, so it ran natively into the real x18 and macOS threw the value away.
//   * the neighbour has a record and hits == 0                      -> control never executed the
//     write; the question is CONTROL FLOW, not storage.
//   * the neighbour has a record and hits >= 1                      -> the write ran and the
//     shadow did not keep it; the write-back is being dropped.
// Printing the current guest word matters as much as the hit count: a record can exist while the
// brk that reaches it has been overwritten by a later mapping of the same text.
void  gmem_x18_trap_dump_near(unsigned long pc, int before, int after);
int   gmem_x18_trap_enter(unsigned long pc, unsigned long long* x, unsigned long* x18,
                          unsigned long long* saved,
                          unsigned long* next);
int   gmem_x18_trap_leave(unsigned long pc, unsigned long long* x, unsigned long* x18,
                          unsigned long long* saved,
                          unsigned long* next);

// Called once from clone(), while the guest is still single-threaded.
//
// BY DEFAULT THIS DOES NOTHING, and that is the point. It used to convert every VENEERED x18 site
// to the trap path, on the premise that "a veneer reads and writes the single global
// gmem_guest_x18 with no way to reach per-thread storage — Darwin has no usable per-thread
// register". That premise was FALSE, and it was reached by measuring the WRONG register:
//   - TPIDR_EL0   is not per-thread here (macOS overwrites it) — that much was right, but
//   - TPIDRRO_EL0 IS per-thread. It is what libpthread's _os_tsd_get_base() reads, and a
//     pthread_key_create()'d key k puts this thread's slot at (tpidrro & ~7) + k*8.
// So a veneer reaches per-thread storage in two extra instructions, and by default now does
// (gmem_x18_tsd_init above). Nothing is left for a clone-time conversion to fix.
//
// It is not deleted, because under RDX18_NOTSD=1 it is still what the shim used to do to every
// image, and that makes it the A/B PERFORMANCE reference for the whole x18 line of work.
// It is NOT a correctness fallback: as of 22129f1 that mode is racy across threads whether or not
// this conversion runs (~619k mismatches on /x18thr either way), because with TSD off every guest
// thread's x18 home is the same global word. See the note in gmem_x18_multithread().
//
// Whether an image is veneered or trapped is decided once, at rewrite time, by
// x18_neutralize_ranges' pass 0 (misaligned SP, or a `br`/`ret x18` no veneer can service).
// This call no longer re-decides it for anybody. It used to be a correctness requirement that it
// not do so per-site, because veneers and the trap path had separate homes for x18; 22129f1 gave
// every guest thread one home addressed by both, so that constraint is gone. What remains is
// simply that a veneer is cheaper than a trap, so there is nothing worth converting.
void  gmem_x18_multithread(void);

// --- fault-driven W^X emulation (RDWXNO=1 disables it and restores the old failing mmap) --------
// arm64 macOS has no simultaneously-writable-and-executable plain memory, and JavaScriptCore asks
// for exactly that in ONE mmap. Serving it as RW and flipping per page on demand is what keeps
// JSC's JIT alive; without it JSC sets useJIT=false and every page load runs on the interpreter.
//
// gmem_wx_fault() is called from the Mach exception handler with the faulting address and whether
// the fault was an instruction FETCH (pc == fault address) rather than a data access. Returns 1
// when it flipped a page and the thread should be resumed at the same pc, 0 when the address is
// not in a W^X range or the page was already in the requested state -- in which case the caller
// must report and die, because a wild pointer must not be retried forever.
int   gmem_wx_fault(unsigned long addr, int instruction_fetch);

// Retire ranges wholly covered by [lo,hi). Called from munmap: a stale range on a recycled
// address would hand the next mapping the dead one's per-page protection bits.
void  gmem_wx_drop(unsigned long lo, unsigned long hi);

// exec flips, write flips, declined faults, x18 sites counted in guest-GENERATED code, pages
// scanned, live ranges. `x18` non-zero means JIT output names x18 and is on the racy path.
// Print the final W^X counts. Called from the exit/exit_group syscall, because the guest's exit
// becomes a host _exit() and atexit() never runs. Silent when no W^X range was ever served.
void  gmem_wx_report(void);

void  gmem_wx_stats(unsigned long long* xf, unsigned long long* wf, unsigned long long* decl,
                    unsigned long long* x18, unsigned long long* pages, int* nranges);


#ifdef __cplusplus
}
#endif
#endif
