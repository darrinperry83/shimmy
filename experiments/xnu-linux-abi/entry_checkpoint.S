/* VM-only entry checkpoint. No stack, globals, calls, or runtime services.
 * Boot_Video offsets are checked against the pinned ARM64 boot.h by the builder.
 * Paint a red 128x64 tile, then halt. This is a deliberately non-booting image.
 */
.text
.globl _rdxnu_entry_checkpoint
_rdxnu_entry_checkpoint:
    bti c
#ifdef RDXNU_ENTRY_POWEROFF
    // Matches AppleVirtualPlatform's ApplePSCI::_systemOff -> _smc_call.
    // An observed guest shutdown is the marker; no framebuffer is required.
    msr daifset, #15
    mov w0, #8
    movk w0, #0x8400, lsl #16
    mov x1, #0
    mov x2, #0
    mov x3, #0
    hvc #0
9:  wfe
    b 9b
#else
    ldr x9, [x0, #40]       // boot_args.Video.v_baseAddr (physical address)
    cbz x9, 9f
    and x9, x9, #0xfffffffffffffffc
    ldr x10, [x0, #56]      // v_rowBytes
    cmp x10, #512
    b.lo 9f
    cmp x10, #16, lsl #12   // <= 65536 bytes per row
    b.hi 9f
    ldr x11, [x0, #64]      // v_width
    cmp x11, #128
    b.lo 9f
    ldr x11, [x0, #72]      // v_height
    cmp x11, #64
    b.lo 9f
    ldr x11, [x0, #80]      // low byte of v_depth
    and x11, x11, #255
    cmp x11, #32
    b.eq 3f
    cmp x11, #30
    b.ne 9f
    mov w12, #0x3ff00000
    b 4f
3:  mov w12, #0x00ff0000
4:
    mov x14, #64
1:  mov x13, x9
    mov x15, #128
2:  str w12, [x13]
    dc cvac, x13
    add x13, x13, #4
    subs x15, x15, #1
    b.ne 2b
    add x9, x9, x10
    subs x14, x14, #1
    b.ne 1b
    dsb sy
9:  wfe
    b 9b
#endif
