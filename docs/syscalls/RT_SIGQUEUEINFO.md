# `rt_sigqueueinfo` (138): checked signal-zero subset

The old path ignored the guest's `siginfo_t`, reported signal-zero success for
any target, and forwarded nonzero guest requests as host signals to the shim.
That could terminate the wrong execution context and could not deliver the
guest payload. This increment checks the native 128-byte `siginfo_t`, validates
the signal and guest target, and permits only a signal-zero existence query for
the current guest process. Unknown guest processes return `ESRCH`; process
groups, child delivery and all nonzero signals return `EOPNOTSUPP`. No host
signal is sent by #138. `rt_tgsigqueueinfo` (#240) is a separate unresolved gap.

Full behavior needs a guest signal queue, `siginfo_t` translation, process and
thread targeting, handler frames and target lifetime protection. This bounded
query does not claim those semantics.

Proof: `./build.sh cli` and `python3 tests/run-rt-sigqueueinfo.py`. The
disposable ARM64 guest checks self signal zero, invalid payload and signal,
unknown and unsupported target forms, and refusal of nonzero signals without
terminating its own shim process. Linux's reference implementation is
[`kernel/signal.c`](https://github.com/torvalds/linux/blob/master/kernel/signal.c).
