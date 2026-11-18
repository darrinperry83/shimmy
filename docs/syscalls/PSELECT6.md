# ARM64 syscall 72: pselect6

The no-mask form uses the host `pselect` wait with checked guest copies of
Linux descriptor bitmaps and timeout. The result sets are copied back only on
success. A zero-descriptor wait does not inspect supplied set pointers. Invalid
timespec components return `EINVAL`. The host supports at most `FD_SETSIZE`
(1024) descriptors, so a larger `nfds` returns `EOPNOTSUPP` instead of silently
ignoring higher numbered descriptors.

Linux ARM64 passes the temporary mask in its sixth argument as an outer
`{mask pointer, size}` pair. The shim checks that pair and the inner 8-byte
mask. A non-null inner mask returns `EOPNOTSUPP` after validation, before
waiting or consuming an event. Shimmy's current virtual signal mask is
process-wide; it cannot supply Linux's atomic, per-thread mask-and-wait
guarantee. A null inner mask uses the normal no-mask path regardless of size.

The raw Linux timeout pointer receives an elapsed-time-adjusted remaining
timeout. Fully virtual signal interruption is not implemented here. The ARM64 guest probe in
`tests/run-pselect6.py` covers ready/empty sets, mask handling, invalid
pointers and timeout validation without changing the installed rootfs.
