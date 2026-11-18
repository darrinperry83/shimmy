# ARM64 `setpgid` (#154) bounded scope

The previous route forwarded both guest numbers directly to Darwin's `setpgid`.
That could select an unrelated macOS PID or process group. It was also wrong
for guest init: Shimmy reports PID 1, while its host PID is different.

This change accepts only the calling guest process (`pid=0` or its guest PID)
creating its own process group (`pgid=0` or the same guest PID). It maps those
forms to host `setpgid(0, 0)` and returns the host's actual success or error.
Negative `pgid` returns `EINVAL`; absent guest PIDs return `ESRCH`. Other known
guest children and group selections return `EOPNOTSUPP` because there is no
verified guest process-group registry for translating them safely.

This is not full process-group compatibility. In particular, parent-directed
`setpgid(child, child)` and joining an existing guest group remain unsupported.
Adjacent `getpgid` (#155) and `getsid` (#156) still need their own guest ID
translation review. The focused ARM64 test covers ordinary and scoped PID-1
identity. A disposable Arch boot reached login with D-Bus settled; it does not
prove every shell job-control path.
