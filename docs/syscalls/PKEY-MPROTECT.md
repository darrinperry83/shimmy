# `pkey_mprotect` (ARM64 syscall 288) bounded subset

The guest has no allocated hardware protection keys. Linux's no-pkeys path
accepts key `0` (the default key) and `-1` (ordinary `mprotect`), and rejects
other keys. Shimmy now maps those two keys to its existing `gmem_mprotect`
translation after checking guest page alignment and the supported R/W/X
protection bits. A zero-length aligned request succeeds before key validation,
matching the Linux order.

This does **not** implement protection-key allocation or per-thread key access.
`pkey_alloc` and `pkey_free` still return `ENOSYS`. The supported keys inherit
the existing `gmem_mprotect` JIT-region caveats; this change does not claim
full Linux VMA semantics or support ARM64 MTE and growth flags.

Evidence: `./build.sh cli` and
`python3 tests/run-pkey-mprotect.py --sysroot <Arch-aarch64-rootfs>` pass in a
disposable guest root. The guest test covers key `0` and `-1` permission updates,
invalid key/alignment/protection rejection without changing a writable mapping,
and zero length.

Primary references: Linux `mm/mprotect.c` `do_mprotect_pkey` and
`include/linux/pkeys.h`'s `!CONFIG_ARCH_HAS_PKEYS` branch.
