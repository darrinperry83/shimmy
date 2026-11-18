# `kcmp` (ARM64 syscall 272) bounded subset

Linux compares *kernel object identity*, not file pathname or inode. Shimmy has
no guest process-resource table or open-file-description identity registry, so
the implementation answers only comparisons whose equality is certain:

- Both PIDs must name the current guest process.
- `KCMP_FILE` returns `0` when both indices name the same open guest fd. It
  checks both fds first and returns `EBADF` for invalid or host-reserved fds.
  Two different live fds return `EOPNOTSUPP`, even if they refer to one file.
- `KCMP_VM`, `KCMP_FILES`, `KCMP_FS`, `KCMP_SIGHAND`, `KCMP_IO`, and
  `KCMP_SYSVSEM` return `0` for the current process compared with itself.
- `KCMP_EPOLL_TFD` and cross-process comparisons return `EOPNOTSUPP`;
  invalid types return `EINVAL` for self comparisons.

This is not full `kcmp` compatibility. In particular, it does not distinguish
duplicated from independently opened fds, report ordering for distinct objects,
or perform Linux's ptrace access checks across processes. Those need an
identity registry and process model; fstat inode equality is insufficient.
The interface follows [Linux `kcmp.c`](https://github.com/torvalds/linux/blob/master/kernel/kcmp.c)
and the [Linux UAPI constants](https://github.com/torvalds/linux/blob/master/include/uapi/linux/kcmp.h).

Proof: `./build.sh cli` and
`python3 tests/run-kcmp.py --sysroot <Arch-aarch64-rootfs>` pass in a disposable
guest root. The test covers same-fd identity, distinct same-inode fd refusal,
invalid fd/type, the self resource classes, and cross-process refusal.
