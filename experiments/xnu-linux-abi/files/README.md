# Native XNU file operations for Linux ELF

This separate `--with-linux-files` candidate extends the working private-file
mapping checkpoint. It reuses the Linux AArch64 open flags, 128-byte stat
layout, checked user-copy behavior and error contracts from `src/syscalls.c`.
XNU owns descriptors, vnode lookup, permissions, open-file references and I/O.

## Integration and update scope

`dispatch.inc` calls native XNU openat, close, lseek, readlinkat, fstat64 and
fstatat64. Linux fcntl currently supports F_GETFD/F_SETFD only. Linux's
AT_FDCWD and open/stat flags are decoded explicitly. Unknown/unported open
flags fail rather than leaking their bit values into Darwin.

`stat_copyout.h` converts native stat64 into the Linux layout, including Linux
device-number packing, full inode/offset widths, timestamps and zero padding.
Two conditional copyout hooks in `kern_descrip.c` and `vfs_syscalls.c` select
this conversion for marked Linux processes. Native Darwin processes retain
their original copyout. Native descriptor/vnode reference lifetimes and
authorization paths are unchanged. No kernel scratch user mappings or separate
file table are introduced.

`files_prepare.py` stages these changes using exact-source anchors and the
existing build lock/restoration workflow. All other personality code remains
in separate files. The previous kernel variants remain available for rollback.

## Verification

```sh
python3 experiments/xnu-linux-abi/files/verify.py --linux-reference
python3 experiments/xnu-linux-abi/build_vm_baseline.py --no-lto --no-bti --with-linux-files
python3 experiments/xnu-linux-abi/link_vm_baseline.py --no-lto --no-bti --with-linux-files
python3 experiments/xnu-linux-abi/stage_vm_variant.py files
```

The freestanding Linux ELF opens the test file itself. Its native parent
supplies a test directory descriptor and current directory, not an already-open
data file. It tests:

- Cwd, directory-relative and absolute paths, including absolute paths ignoring
  an invalid directory descriptor.
- fstat and newfstatat agreement, symlink follow/nofollow, fd EMPTY_PATH,
  readlink truncation and close-on-exec flag inspection/update.
- Reads, EOF, 64-bit seek offsets, opening and inspecting a directory.
- Mapping file code, closing the original descriptor, then executing the map.
- Missing paths, invalid descriptors/pointers/flags, negative seeks,
  O_DIRECTORY on a regular file and O_NOFOLLOW on a symlink.

The parent compares the returned Linux metadata against its own native stat,
checks the exact transcript/exit status and its own identity. The same ELF is
used on reference Linux and XNU. The boot job retains earlier ELF, Arch loader,
PTY, memory and filemap controls, and renders the new result through the Apple
display path. A Linux reference or successful build is not an XNU runtime pass.

Live result: **passed** on kernel UUID `2A795DD5-D68C-30A6-84B0-7237329792AC`,
including the previous ELF/loader/PTY/memory/filemap controls and native parent.
The result was also observed on the Apple display output without WindowServer.
Recorded identities and hashes: [evidence.json](evidence.json).

## Remaining scope

This uses the native filesystem namespace. An isolated Arch rootfs/path policy
is still needed before a service boot. O_PATH, O_DIRECT, O_NOATIME, O_TMPFILE,
empty-path readlink, cwd EMPTY_PATH stat, statx and general fcntl operations are
deferred. The fixture validates read-oriented startup; create/write/truncate
flag conversion exists but is not a filesystem mutation acceptance test.
The fd EMPTY_PATH stat form is implemented; unrelated unported flags return an
error. This is not a full Linux VFS, dynamic linker or systemd runtime pass.

Heap growth and TLS/thread/signal initialization remain prerequisites for
the [systemd target](../systemd/README.md).
