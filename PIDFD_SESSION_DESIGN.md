# Transferable pidfd design for logind sessions

This is a tested primitive, **not** a Shimmy pidfd implementation yet. Normal
Arch login still fails to create `session-c1.scope` and a user manager.

## Why the current path fails

Shimmy's virtual pidfd is a Darwin kqueue. Darwin `sendmsg()` rejects that fd
in `SCM_RIGHTS` with `EINVAL` (reproduced with a native socketpair probe).
systemd v261's logind sends the login leader as the `PIDFDs` property when its
`PidRef` has a fd. The `PIDs` alternative is selected only when the fd is
absent. `pidref_set_pid()` treats a failed `pidfd_open()` as fatal except for
resource errors, and `manager_start_scope()` retries without a pidfd only for
an unknown or read-only D-Bus property. An `SCM_RIGHTS` transport failure
therefore has no honest fallback through these APIs.

Source: [pidref_set_pid](https://github.com/systemd/systemd/blob/v261/src/basic/pidref.c),
[bus_append_scope_pidref](https://github.com/systemd/systemd/blob/v261/src/shared/bus-unit-util.c),
[manager_start_scope](https://github.com/systemd/systemd/blob/v261/src/login/logind-dbus.c).

## Transferable representation

A bound Unix stream socket's accepted endpoint can cross `SCM_RIGHTS` and
retains its bound pathname in `getsockname()` even after unlink. The target
Shimmy **process**, rather than the creator, can own the connected peer.
When the target exits, the kernel closes that peer and the transferred read
endpoint stays pollable at EOF. Its path carries the target PID and exact
kernel-reported process birth seconds/microseconds; recovery must validate
both against `proc_pidinfo()` and reject a dead or reused PID. The target
would receive the peer over a private per-boot control socket and acknowledge
receipt before the creator closes its copy.

`tests/test_transferable_pidfd_lifetime.c` verifies the primitive with three
host processes: creator exits, target still holds peer, observer's fd is not
ready; target exits, observer sees EOF. It also checks retained pathname,
exact birth matching, a wrong birth value, and rejection after target exit.

Before integrating, the control socket needs authenticated per-boot access,
descriptor retention across guest `execve()` and `close_range()`, target
termination races, bounded fd accounting, and tests through real systemd
`StartTransientUnit`. A same-host-user process is not isolated from Shimmy
by this design. Until those checks pass, keep the kqueue pidfd and report
that authenticated logind sessions are incomplete.
