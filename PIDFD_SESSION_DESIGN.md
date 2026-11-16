# Transferable pidfd design for logind sessions

Shimmy now has an opt-in target-owned socket pidfd implementation under
`RDPIDFD_SOCKET=1`. The default kqueue mode is unchanged. The socket mode has
passed native lifetime tests and isolated login with a separate broker queue
compatibility probe, but it is not complete Linux pidfd parity.

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
receives the peer over a private per-boot control socket and acknowledges
receipt before the creator closes its copy.

`tests/test_transferable_pidfd_lifetime.c` verifies the primitive with three
host processes: creator exits, target still holds peer, observer's fd is not
ready; target exits, observer sees EOF. It also checks retained pathname,
exact birth matching, a wrong birth value, and rejection after target exit.

The implemented `tests/test_target_owned_pidfd.c` checks target ownership,
creator exit, SCM_RIGHTS handoff, exec/close_range retention, target exit,
forged birth identity, and descriptor reuse. An isolated full Arch login
created a logind scope and user manager only with this mode **and**
`RDSOCKETOUTQ_COMPAT=1`; the latter is a separate experimental D-Bus broker
compatibility probe.

The control socket checks the kernel peer PID and same Shimmy image, and
requires a private socket directory. This does not isolate another process
running as the same macOS user. Nonzero `pidfd_send_signal` is unsupported in
socket mode to avoid signaling a reused host PID. The registry has a fixed
512-slot capacity, and some default kqueue-mode fallbacks do not match Linux
errors. Keep this representation opt-in until those semantics and desktop
sessions are verified.
