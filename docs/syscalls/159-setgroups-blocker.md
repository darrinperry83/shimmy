# ARM64 syscall 159: setgroups blocker

The current `RDUID0` handler returns success without changing supplementary
groups. The non-`RDUID0` handler forwards `setgroups` to Darwin, which changes
the shim process's host credentials rather than a private Linux credential.
Neither path is a safe Linux implementation.

An isolated prototype (`syscall/159-setgroups`, commit `78d0eb9`) stores a
guest-private list across fork and shim-mediated exec. It passes an ARM64
probe for set/clear, invalid guest memory and virtual-root permission checks.
That prototype is **withheld**. Linux `setgroups` affects filesystem access,
while Shimmy still executes file operations under the Mac process's host
supplementary groups. Returning success for `setgroups(0, NULL)` would tell
the guest its groups were dropped even though host group access remains.
Making `getgroups` echo the virtual list only hides that mismatch.

Before enabling this call, the guest VFS must enforce Linux UID/GID and
supplementary-group permissions for all path and fd operations, including
fork/exec inheritance, ownership metadata, and access after privilege drop.
The capability rules for `CAP_SETGID` must also be coherent. A regression
gate should create a host-group-accessible file, clear the guest's groups,
then prove access is denied through every relevant guest file operation.
Systemd login and desktop boot must pass with truthful behavior. Until then,
do not count syscall 159 as working or merge the isolated runtime branch.
