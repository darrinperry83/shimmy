/* P0 experiment, included only by the patched osfmk/arm64/sleh.c.
 * Not a Linux process personality. See DESIGN.md before extending this.
 * Uses XNU's saved-state accessors, proc_pid(), and boot-argument parser.
 */
#ifndef RDXNU_LINUX_PROBE_H
#define RDXNU_LINUX_PROBE_H

#define RDXNU_PROBE_MARKER 0x4c58
#define RDXNU_LINUX_GETPID 172
#define RDXNU_LINUX_ENOSYS 38

#if DEBUG || DEVELOPMENT
/* Match XNU's narrow Mach-to-BSD declaration in osfmk/vm/vm_protos.h.
 * Including BSD sys/proc.h here is invalid in the MACH_KERNEL translation unit.
 */
extern int proc_pid(struct proc *);
#endif

static bool
rdxnu_linux_probe(arm_saved_state_t *state, struct proc *p)
{
#if DEBUG || DEVELOPMENT
	int enabled = 0;
	/* SVC #0 + x16 marker opts in without changing any Darwin process state.
	 * x16 is a temporary laboratory convention, NOT part of the Linux ABI.
	 */
	if (!is_saved_state64(state) ||
	    (get_saved_state_esr(state) & 0xffff) != 0 ||
	    get_saved_state_reg(state, 16) != RDXNU_PROBE_MARKER) {
		return false;
	}
	if (!PE_parse_boot_argn("rdxnu_linux_probe", &enabled, sizeof(enabled)) ||
	    enabled != 1) {
		return false;
	}
	/* Linux AArch64: number in x8; signed result/negative errno in x0.
	 * Leave PC, other registers and flags to the normal exception-return path.
	 * There is no userspace exception handler or syscall instruction rewrite.
	 */
	uint64_t nr = get_saved_state_reg(state, 8);
	uint64_t result = nr == RDXNU_LINUX_GETPID ?
	    (uint64_t)proc_pid(p) : (uint64_t)(-(int64_t)RDXNU_LINUX_ENOSYS);
	set_saved_state_reg(state, 0, result);
	return true;
#else
	(void)state;
	(void)p;
	return false;
#endif
}
#endif
