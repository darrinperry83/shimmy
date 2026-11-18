# ARM64 `sched_getattr` (#275) checked output

The Linux v0 `sched_attr` result is built in a host-local buffer. Its 48-byte
v0 fields and zeroed extension through the caller's validated size (up to
4096 bytes) are copied to guest memory in one checked operation. An invalid
or partly protected output returns `EFAULT` without writing its valid prefix.
The existing self-thread, policy and size restrictions are unchanged.

`tests/run-sched-getattr-output.py` builds a disposable raw ARM64 guest. The
probe checks v0 fields, zero extension bytes, a 48-byte stack output without
touching its sentinel, invalid and read-only output, a result crossing into a
protected page without partial write, and existing size/flags/target errors.

The focused probe and CLI build passed. A private Arch boot reached a fresh
login prompt with D-Bus settled; the exact guest and rootfs clone were
cleaned. This increment does not add RT/deadline scheduling or
claim process-wide priority as an exact per-thread Linux scheduler model.
