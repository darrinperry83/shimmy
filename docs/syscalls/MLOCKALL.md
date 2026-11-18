# `mlockall` (ARM64 syscall 230): host passthrough risk and guest dependency

The current handler in `src/syscalls_ext.c` passes Linux `MCL_*` flags directly to Darwin `mlockall()`. This does **not** establish complete Linux guest semantics. A successful host call locks the whole Shimmy process, including host code, JIT mappings and buffers outside the guest. The host may also return `ENOSYS`; `GREETER_BOOT.md` records that behavior on this Mac. There is no guest-wide lock registry or future-map policy, and `munlockall` (#231) is unsupported. No part of this call has been verified as complete guest `mlockall` compatibility.

The boot dependency matters: stock greetd 0.10.3 calls `mlockall(MCL_CURRENT | MCL_FUTURE)` and aborts when it fails. Replacing the host call with an honest refusal would remove the process-wide side effect, but would **not** make greetd work. Changing the runtime response without a guest lock implementation requires a private Arch boot and greeter regression gate; the isolated refusal prototype is not part of this documentation commit.

Full support needs enumeration of every guest mapping, including SysV shared memory and device-backed mappings, atomic locking and rollback for `MCL_CURRENT`, a lock policy applied to every subsequent guest mapping for `MCL_FUTURE`, and per-guest state that `munlockall` can release without unlocking unrelated host memory. `MCL_ONFAULT` also needs a fault-aware path. Existing bounded per-range `mlock` (#228) and eager `mlock2` (#284) do not satisfy those process-wide requirements.

Until that model and its boot tests exist, treat #230 as a blocker for the stock greetd GUI path, not a completed syscall. Avoid using host `mlockall` success as evidence that the Linux guest's pages are locked correctly.
