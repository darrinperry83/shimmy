# Path descriptors and basic statx on XNU

Live kernel `4977C3F3-284E-322C-86F7-39C7FA62F17C` passes this fixture and
all earlier controls. Unchanged Arch systemd 262.2-1 prints the exact Linux
reference version output, exits zero, and has empty stderr. Output is visible
through AppleParavirtDisplay with WindowServer absent. See [evidence.json](evidence.json).

The isolated `--with-linux-paths` variant extends the startup checkpoint with
Linux O_PATH, basic statx, empty-path readlinkat, and bounded fcntl operations.
The existing shim supplies the Linux flag, layout, and error contracts; XNU
owns lookup, credentials, descriptors, vnode references, and metadata.

O_PATH creates a native fileproc/fileglob holding a plain vnode reference. It
does not call VNOP_OPEN or request data access, so unreadable files and FIFOs
can be inspected without opening them for I/O. Parent-directory search
permissions still apply. A plain reference keeps an unlinked inode alive;
an event-only reference would permit recycling and is unsuitable here. Native
duplication, descriptor flags, and close manage the lifetime. Read, write,
seek, mapping, and unsupported path-descriptor fcntl operations return EBADF.

O_DIRECTORY, O_NOFOLLOW, and O_CLOEXEC are retained; other O_PATH open flags
are ignored as on Linux. F_GETFL returns the retained Linux flags. F_GETFD,
F_SETFD, F_DUPFD, and F_DUPFD_CLOEXEC use native descriptor operations.
Empty-path readlinkat reads the held symlink itself. Basic statx supports
relative paths, symlink policy, and AT_EMPTY_PATH, including the current
directory. Its 256-byte Linux structure reports basic fields and a mount ID
from XNU's `vfs_mount_id(vnode_mount(vp)) + 1`, captured under the same vnode I/O
reference used for metadata. XNU's initial root mount has ID zero; adding one
keeps a positive ID for that mount and preserves distinct identities. Other
mounts receive XNU's system-wide counter; this is not a device number.
STATX_MNT_ID is reported; the newer unique-ID mask,
birth time, and attributes remain unclaimed. Explicit FORCE_SYNC/DONT_SYNC
modes remain unsupported.

Systemd 262.2's [xstatx_full](https://github.com/systemd/systemd/blob/v262.2/src/basic/stat-util.c)
returns EUNATCH when a required mount-ID field is absent. A basic-statx-only
candidate passed the descriptor fixture but produced that warning from systemd;
the native mount-ID backend addresses the missing field without masking errors.

APFS invalidates the target text on unlink even for a normal Darwin O_SYMLINK
descriptor (the inode, size, and zero link count remain available). The Linux
descriptor therefore captures immutable symlink text when it acquires the vnode.
A per-symlink operations object owns that snapshot; fileglob layout and vnode
data stay unchanged. Dup shares it, and the final native close frees it. Readlink
still checks the caller's native MAC and vnode authorization. This gate does not
qualify a concurrent unlink during acquisition or filesystems with mutable
synthetic symlink text; snapshot acquisition itself can fail under native policy.
The APFS difference is reproducible with `native_link_control.c`, compiled with
the host's `xcrun clang`. It must report a retained inode, zero links, size four,
and Darwin EINVAL from the post-unlink freadlink call.

The unchanged Linux ELF fixture runs with uid/gid 65534 after a root native
parent creates fixtures. It checks permissions, a FIFO, directory-relative
lookup, ignored create/truncate flags, descriptor status, duplication, metadata
and error paths. While the child blocks, the parent unlinks and replaces both
a file and symlink. The held descriptors must still identify the original
inodes, report zero links, and read the original symlink target. The parent
also compares Linux metadata against native metadata and requires zero exit.
Mount identity must match between a path and its held descriptor, remain stable
after unlink, and differ between the fixture's filesystem and `/dev/null`.

```sh
python3 experiments/xnu-linux-abi/paths/verify.py --linux-reference
python3 experiments/xnu-linux-abi/build_vm_baseline.py --no-lto --no-bti --with-linux-paths
python3 experiments/xnu-linux-abi/link_vm_baseline.py --no-lto --no-bti --with-linux-paths
python3 experiments/xnu-linux-abi/stage_vm_variant.py paths
```

Install only the uniquely named candidate printed by the stager in the
existing disposable VM. After boot, verify its UUID, prior regression gates,
path fixture, exact systemd version output, and empty systemd stderr with:

```sh
python3 experiments/xnu-linux-abi/startup/verify_runtime.py <staged-name>
```

This does not establish complete Linux VFS/fcntl semantics, procfs descriptor
reopening, Linux rootfs isolation, PID 1, a service manager, or physical drivers.
Normal-descriptor F_GETFL translation covers bounded common access/status bits;
not every Linux status bit is preserved. Poll/select behavior is not qualified.

`trace.gdb` records openat/readlinkat/newfstatat/statx/fcntl arguments and results
from the original Arch systemd on Linux. Use the immutable image recorded in
`out/systemd/reference.json`, mount `out/systemd` at `/capture` and this script
read-only, and run GDB against the copied Arch loader as in
`systemd/reference.py`. This revealed O_PATH component traversal and statx's
0x5109 mask before implementing the backend. The Linux reference uses Docker
only as a control; the runtime implementation executes directly on XNU.
