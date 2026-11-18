# `prlimit64` boot blocker (ARM64 syscall 261)

The published `prlimit64` route is not a full Linux implementation. It accepts
guest pointers as host pointers and reports unknown resources as unlimited.
An isolated branch (`syscall/261-prlimit64`, `b869603`) added checked guest
buffers, self-only PID targeting, truthful unsupported-resource errors, and
the 4096 guest `RLIMIT_NOFILE` soft cap used by `getrlimit`/`setrlimit`.
Its disposable ordinary and PID 1 ARM64 probes and CLI build passed.

The isolated branch **failed the private Arch boot gate**: a login prompt
appeared, but the D-Bus broker and launcher did not settle. A combined boot
without #261 passed. A temporary diagnostic run on the isolated branch showed
guest PID 1 calling `prlimit64(pid=0, RLIMIT_NOFILE=7,
new={soft=1048576, hard=UINT64_MAX})`. The proposed 4096 cap rejected this
request with `EOPNOTSUPP`. That is the concrete conflict; no unsupported
resource or cross-PID call was observed in the diagnostic boot. The private
clone and diagnostic instrumentation were removed afterward. The rootfs
clone reported permission warnings for protected systemd fixture entries,
so this remains a boot smoke test, not a full filesystem fidelity result.

Do not merge the bounded branch as a working syscall. Quietly accepting the
million-descriptor request while continuing to report 4096 would also give
the guest contradictory limit state. The correct follow-up is to make the
guest-visible NOFILE state coherent across `getrlimit`, `setrlimit`, and
`prlimit64`, then address the performance cost of close-before-exec loops
through an efficient descriptor/`close_range` path. Retest systemd login,
D-Bus, representative subprocess startup, and the limit readbacks together.
