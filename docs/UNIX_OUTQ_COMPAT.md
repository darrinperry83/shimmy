# Experimental Unix socket output queue probe

Native session mode automatically enables a fallback for Linux `TIOCOUTQ`/`SIOCOUTQ`
when Darwin returns `EOPNOTSUPP` on an AF_UNIX socket. D-Bus broker queries
this value after forwarding a message containing SCM_RIGHTS descriptors. The
previous Darwin result caused broker to close the session connection.

The fallback uses the kernel peer PID and socket identity to inspect the
receiver's queue charge. It returns an error if the peer cannot be identified
or inspected. Native tests cover an empty queue, data, one and two queued
descriptors, receipt, invalid descriptors, and a separate receiver process.
An isolated full Arch login test on 17 November 2026 confirmed the broker
stayed up, the user manager started, and the user bus responded when this was
combined with the native-session pidfd transfer path. That path converts kqueue-backed
pidfds to transferable target-owned socket descriptors only at `SCM_RIGHTS`
handoff; see `LOGIND-SESSION-PIDFD.md`.

The macOS receive-buffer charge is not byte-identical to Linux socket output
accounting, and peer inspection may fail when endpoints are closing or moving.
`run.sh --init` selects native session mode, so no output-queue flag is needed
for the tested systemd boot path. Standalone sessions keep the old behavior;
`RDSOCKETOUTQ_COMPAT=0` can disable the translation for diagnostics. This is not a general Linux socket quota
implementation.
