# Logind session handoff in the Arch boot

On 17 November 2026, an isolated full Arch boot reached getty and authenticated
`shimtest`, but `pam_systemd` returned `io.systemd.System/EINVAL`, leaving
`XDG_RUNTIME_DIR` empty. The intact `CreateSession` request reached logind and
peer-credential registry lookups succeeded. Logind's debug journal identified
the real failure: pushing the session leader pidfd and starting
`session-c1.scope` both returned `EINVAL`.

Shimmy's local pidfd is a Darwin kqueue. A native `SCM_RIGHTS` probe confirmed
Darwin rejects passing a kqueue descriptor with `EINVAL`. The new
`RDPIDFD_TRANSFER=1` path keeps kqueues for local monitoring and substitutes a
target-owned Unix socket pidfd only when a pidfd is sent with `SCM_RIGHTS`.
The native `test_hybrid_pidfd_transfer.c` checks the rejected kqueue send, the
successful socket send, peer identity, and exit readiness.

With that path alone, dbus-broker forwarded the descriptor but exited after
the transfer. Its queued-output inspection needs Shimmy's existing
`RDSOCKETOUTQ_COMPAT=1` path. With both enabled in a disposable root using the
original dbus-broker unit, the real getty/login/PAM flow reported:

```
LOGIN_PROMPT
LOGIN_SHELL_AND_ID
LOGIN_USER_SESSION_OK
LOGIN_USER_BUS_OK
```

The journal also recorded `New session 'c1' of user 'shimtest' with class
'user' and type 'tty'` and `Started Session c1 of User shimtest`. The shell
reported `XDG_RUNTIME_DIR=/run/user/22001`. `run.sh --init` supplies both
flags and a private per-boot socket namespace. The live root and running
desktop were not changed by this test.

This proves authenticated TTY session creation in the isolated root. It does
not prove SDDM, a visible Plasma desktop, lock/unlock, or the full Linux pidfd
contract. The socket representation cannot safely deliver nonzero
`pidfd_send_signal` without a target-owned signaling protocol; existing code
returns `ENOTSUP` in socket-only mode. Darwin's socket output charge is also
not byte-identical to Linux's accounting. Keep those limits explicit in later
desktop tests.

## Runtime subdirectory ownership

The per-user tmpfs mount fallback itself was not the D-Bus failure: the
disposable `guest_tmpfs_mount` test returned `EPERM` for both pathname and
`/proc/self/fd/N` runtime targets, then chown/stat reported the requested
virtual owner. A separate guest test reproduced the D-Bus failure after
login: a process with virtual UID/GID 22001 created
`/run/user/22001/dbus-1`, but stat reported UID/GID 0 because `mkdirat`
created a host-owned directory without publishing virtual ownership.

`mkdirat` now records the non-root guest creator's UID/GID in the existing
inode-keyed ownership sidecar. The focused guest test reports
`runtime child uid=22001 gid=22001 mode=700`. In a fresh isolated full Arch
login, `stat` reported `/run/user/22001` as `22001:22001:700`, the user
manager started, and `busctl --user list` succeeded. A separate GUI probe
reached KWin and Plasmashell processes with XDG_RUNTIME_DIR set; it did not
prove a correctly rendered desktop or resolve the KWin frame stall. Both
probes used a disposable root and throwaway account; the installed root was
untouched.
