# `execveat` (#281): checked guest pathname

The existing `execveat` handler handled relative paths, `AT_EMPTY_PATH`,
`AT_SYMLINK_NOFOLLOW`, and the host shim re-exec bridge. It read the guest
pathname directly, so an invalid Linux pointer could crash the host shim
before the syscall returned. The handler now snapshots at most `PATH_MAX`
bytes through the guest memory API before testing the path or resolving it.
Invalid addresses return `EFAULT`; a path with no terminator within the
bound returns `ENAMETOOLONG`. Existing flag and empty-path behavior is kept.

`./build.sh cli` passed. The disposable ARM64 `tests/run-execveat.py` probe
passed all eight modes, including the new invalid-pointer case. A separate
private Arch clone reached a fresh login prompt with the D-Bus broker and
launcher settled; the harness stopped its exact guest and the clone was
removed. This is a pathname safety correction, not proof of full Linux
`execveat` compatibility or GUI boot.
