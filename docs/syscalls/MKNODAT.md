# `mknodat` (ARM64 syscall 33) bounded compatibility

Shimmy creates regular files and FIFOs inside its guest root using the host
filesystem. This change matches Linux's `may_mknod()` file-type validation:
`S_IFDIR` returns `EPERM`, while unsupported file types such as `S_IFLNK`
return `EINVAL`, before path resolution. Creation of a character or block
device remains refused with `EPERM` and does not touch a host device node.

The [Linux implementation](https://raw.githubusercontent.com/torvalds/linux/master/fs/namei.c)
is in `may_mknod()` and `filename_mknodat()`. Linux also permits an unbound
`S_IFSOCK` inode. Shimmy does not yet implement that: Darwin `mknod()` returned
`EPERM` for an isolated socket-inode probe, so Shimmy continues to refuse it.
Ownership, ACL, idmapped mount, and all path-race details remain unverified.

Proof: `./build.sh cli` and `python3 tests/run-mknodat.py --sysroot
<Arch-aarch64-rootfs>` pass using a disposable guest root. The guest test
covers regular files, FIFOs, directory/invalid/device type errors, and an
existing symlink, with no device creation.
