# `setfsgid` (ARM64 syscall 152): credential-model blocker

The current route in `src/syscalls_ext.c` returns the effective guest GID
without using the requested GID. This happens to describe a no-op call when
the filesystem GID already equals the effective GID, but does not implement a
change of filesystem GID. A caller can ask for a different GID, receive the
old one, and later observe that the requested value never took effect.

`src/syscalls.c` stores only real, effective and saved guest GIDs, exports
those six UID/GID values through the shim's exec environment, and publishes
effective credentials to the peer registry. There is no separate filesystem
GID to preserve across fork and exec. More importantly, file access still
uses the host process's Darwin credentials, while `guest_ownership.c` states
that its virtual ownership records are compatibility metadata rather than a
guest security boundary. Adding an `fsgid` variable and returning it would
therefore claim a file-access effect that does not exist.

Implement #152 alongside a guest credential and filesystem-permission model:

1. Track `fsgid` separately from real, effective and saved GIDs, including
   Linux transition rules and fork/exec inheritance.
2. Use that value for guest VFS permission checks on path and descriptor
   operations, with virtual ownership, supplemental groups and ACLs handled
   consistently. Keep the host process's credentials unchanged.
3. Test a successful transition, a denied transition, and file access before
   and after the transition using a disposable ARM64 root. Then run the
   private Arch login and D-Bus boot gate.

This branch changes no runtime behavior. It must not be counted as a working
syscall or merged merely to raise the dispatch coverage count.
