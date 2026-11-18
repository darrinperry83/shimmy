# `syslog` (ARM64 syscall 116): remove false success

Shimmy previously returned zero for every `syslog` action without a Linux kernel printk ring. That made `READ_ALL` look like an empty successful read, `SIZE_BUFFER` look like a zero-capacity buffer, and privileged control operations appear to succeed. The shim's `/dev/kmsg` transport is a per-boot guest logging path, not the Linux printk ring that `syslog` reads and clears.

This branch returns `ENOSYS` for `READ_ALL` (3) and `SIZE_BUFFER` (10), which Linux can permit without `CAP_SYSLOG` when `dmesg_restrict` is off. Every other action returns `EPERM`: Linux checks `CAP_SYSLOG` before action-specific validation, and Shimmy's current `capget` reports no capabilities even with `RDUID0`. The syscall does not write the guest's buffer. This is a truthful unsupported result, **not** a full `syslog` implementation.

`./build.sh cli` and `python3 tests/run-syslog.py` exercise the ARM64 arguments, permission and unsupported cases, and no-write guarantee in a disposable root. A full implementation needs a bounded guest printk ring shared with `/dev/kmsg`, Linux record formatting, read cursor, clear state, size reporting, and a coherent capability policy. It must never expose the host's kernel log.

Primary source: [Linux `kernel/printk/printk.c`, `do_syslog` and permission checks](https://github.com/torvalds/linux/blob/master/kernel/printk/printk.c).
