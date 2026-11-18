# `fchownat` (AArch64 syscall 54): checked guest pathname

Both the virtual-root ownership path and the host-identity path previously
inspected or resolved a raw guest pathname pointer. An unmapped nonzero
pointer could crash the shim instead of returning `EFAULT`. The handler now
copies a non-NULL pathname through the existing bounded guest-memory helper
before either branch uses it. Its existing NULL plus `AT_EMPTY_PATH` behavior
is preserved.

The disposable ARM64 probe checks invalid nonzero pointers in both modes.
In virtual-root mode it also exercises a named file, an empty pathname, and
NULL with `AT_EMPTY_PATH`; the host file's UID and GID are checked unchanged.
No installed rootfs file or real host credential is changed.

This focused path fix does not establish Linux ownership security. Virtual
ownership records are compatibility metadata, not enforcement of guest DAC
over Darwin-backed files; descriptor provenance, symlink races, and broader
flag behavior remain separate work.
