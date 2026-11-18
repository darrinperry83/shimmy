# `umask` (#166)

The Linux guest mask is limited to `S_IRWXUGO` (`0777`) before calling
Darwin's process-local `umask`. Darwin retained additional special-mode bits
from `umask(07777)`, while Linux's syscall stores only the nine permission
bits. The previous implementation passed the raw argument through.

The disposable ARM64 probe checks the previous-mask return value, stripping
of bits above `0777`, process inheritance and parent isolation across
`clone(SIGCHLD)`, and `mkdirat` results: a child mask of `077` creates mode
`0700`, while the parent's surviving `022` creates mode `0755`. `./build.sh
cli` and `python3 tests/run-umask.py` passed on 2026-11-18.

This uses the host shim process's umask because each Linux guest process is a
separate host process; it does not provide a separate mask per guest thread or
model interactions with host-side helper file creation. The test does not
establish every filesystem permission rule or a complete systemd boot.
