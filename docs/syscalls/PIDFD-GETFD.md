# `pidfd_getfd` (ARM64 syscall 438): bounded self-file subset

Shimmy duplicates a regular file descriptor opened beneath the guest root only
when the pidfd identifies the current host process and is the kqueue-backed
pidfd representation. The new descriptor shares file offset and status with the
original open file description and has `FD_CLOEXEC`, matching Linux's core
behavior. Nonzero flags return `EINVAL`; an invalid pidfd or target fd returns
`EBADF`.

Cross-process copies and socket-backed pidfds return `EOPNOTSUPP`. Synthetic
devices, sockets, directories, host paths outside the guest root, and other
nonregular targets also return `EOPNOTSUPP`. Shimmy currently has no complete
guest-fd ownership registry or cross-process descriptor transfer service, so
those cases cannot safely share this implementation. The guest PID 1 uses a
different self PID mapping and is not covered by this subset.

The [Linux source](https://raw.githubusercontent.com/torvalds/linux/master/kernel/pid.c)
shows the flag check, ptrace-gated target lookup, open-file-description transfer,
and unconditional close-on-exec behavior. Proof is `./build.sh cli` plus
`python3 tests/run-pidfd-getfd.py --sysroot <Arch-aarch64-rootfs>` against a
disposable root. This is not a general `pidfd_getfd` implementation.
