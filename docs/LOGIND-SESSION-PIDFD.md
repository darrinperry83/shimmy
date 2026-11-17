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
