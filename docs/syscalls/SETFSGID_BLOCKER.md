# `setfsgid` (ARM64 syscall 152): credential-model blocker

The previous route in `src/syscalls_ext.c` returned the effective guest GID
without using the requested GID. A caller could ask for a different GID,
receive the old one, and later observe that the requested value never took
effect. The bounded route now returns that value only for the `gid_t` value
`-1` (query) or a request equal to the current effective GID (no change).
A different request returns `EOPNOTSUPP` without changing host or guest
credentials. Linux normally returns the previous fsgid even for a denied
request, so this explicit refusal is a compatibility limit, not a complete
implementation.

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

The ARM64 guest probe checks query/no-change, refused changes, and a virtual
`setgid` transition under `RDUID0`; no host credential change is attempted.
The isolated CLI build and probe passed. A private Arch boot reached a fresh
login prompt with the D-Bus broker and launcher settled; the harness stopped
its scoped guest and the exact clone was removed. The fixture copy emitted
protected-path permission warnings, so this is a boot smoke test. Do not
count the bounded subset as full `setfsgid` compatibility or an effective
filesystem-permission switch.
