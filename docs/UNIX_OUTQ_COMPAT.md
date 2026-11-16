# Experimental Unix socket output queue probe

`RDSOCKETOUTQ_COMPAT=1` enables a fallback for Linux `TIOCOUTQ`/`SIOCOUTQ`
when Darwin returns `EOPNOTSUPP` on an AF_UNIX socket. D-Bus broker queries
this value after forwarding a message containing SCM_RIGHTS descriptors. The
previous Darwin result caused broker to close the session connection.

The fallback uses the kernel peer PID and socket identity to inspect the
receiver's queue charge. It returns an error if the peer cannot be identified
or inspected. Native tests cover an empty queue, data, one and two queued
descriptors, receipt, invalid descriptors, and a separate receiver process.
An isolated full Arch login test must additionally confirm broker stays up,
the user manager starts, and the user bus responds.

The macOS receive-buffer charge is not byte-identical to Linux socket output
accounting, and peer inspection may fail when endpoints are closing or moving.
The flag remains off by default. This is not a general Linux socket quota
implementation.
