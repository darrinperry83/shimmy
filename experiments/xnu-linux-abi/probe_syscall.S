/* Mach-O carrier; the marked entry contains the real Linux SVC instruction.
 * x16 marker is P0-only. No instruction rewriting or exception interception.
 */
.text
.p2align 2
.globl _rdxnu_probe_syscall
_rdxnu_probe_syscall:
    mov x8, x0
    mov x0, #0x1234
    mov x16, #0x4c58
    svc #0
    ret

/* Control: ordinary Darwin getpid with the same SVC immediate.
 * The marker, not the immediate alone, selects the P0 path.
 */
.p2align 2
.globl _rdxnu_darwin_getpid
_rdxnu_darwin_getpid:
    mov x16, #20
    svc #0
    ret
