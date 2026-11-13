// vdso.h — synthesized Linux aarch64 vDSO so libc gets trap-free time.
//
// We build a tiny in-memory ELF exporting the standard aarch64 vDSO symbols
// (__kernel_clock_gettime / __kernel_gettimeofday / __kernel_clock_getres /
//  __kernel_time / __kernel_getcpu). Their code reads the macOS commpage-backed
// clock instead of trapping, so hot time calls cost ~ns instead of a Mach round trip.
#ifndef RDRIFT_VDSO_H
#define RDRIFT_VDSO_H
#ifdef __cplusplus
extern "C" {
#endif

// Build the image (executable, via gmem). Call once, after gmem_init().
void vdso_build(void);

// Base address to advertise as AT_SYSINFO_EHDR; 0 if not built (libc then falls back to syscalls).
unsigned long vdso_auxv_base(void);

#ifdef __cplusplus
}
#endif
#endif
