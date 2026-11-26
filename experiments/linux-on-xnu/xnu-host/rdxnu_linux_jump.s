/* Context stays live while f runs. Store the incoming raw LR, not a signed
 * return address. All C objects use arm64e, including the function pointer f.
 * Kernel C uses no FP/SIMD state (-mno-implicit-float); do not touch that state
 * from the kernel, where FP access can trap and is not scheduler-preserved.
 */
.text
.p2align 2
.globl _rdxnu_linux_jump_set
_rdxnu_linux_jump_set:
    stp x19, x20, [x0, #0]
    stp x21, x22, [x0, #16]
    stp x23, x24, [x0, #32]
    stp x25, x26, [x0, #48]
    stp x27, x28, [x0, #64]
    stp x29, x30, [x0, #80]
    mov x9, sp
    str x9, [x0, #96]
    mov x19, x0
    blraaz x1
    mov x0, x19
    b _rdxnu_linux_jump_back
.globl _rdxnu_linux_jump_back
_rdxnu_linux_jump_back:
    ldr x9, [x0, #96]
    ldp x19, x20, [x0, #0]
    ldp x21, x22, [x0, #16]
    ldp x23, x24, [x0, #32]
    ldp x25, x26, [x0, #48]
    ldp x27, x28, [x0, #64]
    ldp x29, x30, [x0, #80]
    mov sp, x9
    ret
.subsections_via_symbols
