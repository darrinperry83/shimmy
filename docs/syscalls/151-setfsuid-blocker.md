# `setfsuid` (#151): credential and filesystem authorization blocker

Status: **not implemented as a Linux filesystem UID switch**. Do not count its
existing dispatch case as compatible.

The current handler in `src/syscalls_ext.c` returns the guest effective UID
(`geteuid()` without `RDUID0`) and ignores the requested UID. Linux returns the
previous filesystem UID whether or not a change succeeds, so that return value
alone cannot prove a successful switch. A root guest can currently request a
different filesystem UID and receive the apparent normal return while all
subsequent filesystem operations continue under the old identity.

The guest credential state in `src/syscalls.c` has only real, effective and
saved IDs (`g_guest_uid[3]`). The `mkdirat` virtual ownership path uses the
effective UID. Other file access still reaches Darwin's host DAC under the
Mac user's UID. Adding a separate `g_guest_fsuid` and returning it would make
the ABI look better but would not cause either access checks or new-file
ownership to follow that ID. Changing the host process UID would cross the
guest/host security boundary and is not an acceptable shortcut.

Before claiming #151 works, the credentials subsystem needs:

1. An explicit per-guest filesystem UID with Linux transition rules across
   `set*uid`, fork and exec.
2. Guest ownership and permission checks for relevant path and fd operations,
   with file creation using fsuid instead of euid.
3. Isolation from the host UID and another guest instance, plus tests for
   allowed and disallowed changes, parent/child inheritance, and file access.
4. A private Arch login/D-Bus boot regression after integration.

Until then, the existing case is a compatibility expedient, not a working
syscall. No runtime behavior is changed by this note.
