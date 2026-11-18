# ARM64 #50 `fchdir`: guest working directory

The previous extended handler called Darwin `fchdir(fd)` and returned success
without updating Shimmy's guest `g_cwd`. Guest `getcwd` and all subsequent
relative path resolution therefore kept using the old directory.

The core handler now resolves the opened directory FD to a guest path, then
checks that resolving that guest path leads back to the same device and inode.
This prevents an inherited host directory FD outside the rootfs from silently
becoming a different directory inside the guest. It verifies search access
before updating `g_cwd`; invalid and reserved descriptors remain errors. The
host process working directory is not changed because Shimmy's guest resolver
uses `g_cwd` rather than the host cwd.

The subset requires a named directory whose guest path still resolves to the
opened inode. A deleted or replaced directory FD can return an error instead
of reproducing every Linux corner case. Mac passthrough paths explicitly
enabled through `RDSHARE` remain eligible when they resolve to the same inode.

Proof: `./build.sh cli`; `python3 tests/run-fchdir.py` runs a disposable ARM64
guest that checks `getcwd`, relative open, file/invalid FD errors and a passed
host directory outside the guest root. The latter uses `RDSHARE=` so it cannot
be treated as an authorized Mac share. A private Arch login/D-Bus smoke test
checks boot compatibility, not all directory and mount semantics.
