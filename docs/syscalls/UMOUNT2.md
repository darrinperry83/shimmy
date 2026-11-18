# umount2 (ARM64 syscall 39)

Shimmy currently synthesizes early API filesystem mounts without a mutable
guest mount table. Returning success from `umount2` never detached anything;
callers could believe `/proc` or `/dev` was gone while it remained visible.

This bounded handler validates Linux flag bits and reads the guest path through
a checked, length-limited copy. It resolves the path within the guest root,
returns `ENOENT` for a missing target, and refuses an existing target with
`EPERM`. It never calls host `unmount` or changes a macOS volume. A future
implementation needs private guest mount state plus consistent path lookup,
mountinfo, and statfs views before a successful detach is safe.

During normal systemd boot, the current `mount(40)` behavior remains as it was.
The refusal only changes callers that invoke `umount2`, typically cleanup or
shutdown paths. In a private Arch clone, the CLI reached a fresh login prompt
with D-Bus broker and launcher settled; this does not verify shutdown behavior.
Run `./build.sh cli` and `python3 tests/run-umount2.py` for the disposable ARM64
syscall test. The private boot guest and its exact clone were removed afterward.
