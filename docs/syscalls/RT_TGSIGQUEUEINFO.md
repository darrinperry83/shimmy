# `rt_tgsigqueueinfo` (240): checked current-thread signal-zero subset

The former handler ignored guest `siginfo_t`, reported signal-zero success for
any target, and could send a nonzero host signal to the shim. This increment
checks the native 128-byte guest payload and positive guest process/thread IDs.
Only a signal-zero existence query for the current guest process and current
guest thread succeeds. Unknown IDs return `ESRCH`; known but unsupported
cross-thread/process targets and all nonzero signals return `EOPNOTSUPP`.
No host signal is sent by #240.

This does not implement guest thread signal queues, `siginfo_t` delivery,
handler frames or realtime semantics. Linux's reference implementation in
[`kernel/signal.c`](https://github.com/torvalds/linux/blob/master/kernel/signal.c)
copies `siginfo_t` before resolving the target; the shim likewise rejects an
invalid payload before reporting on the target.

Proof: `./build.sh cli` and `python3 tests/run-rt-tgsigqueueinfo.py`. The
disposable ARM64 guest checks the current PID/TID, invalid payload, invalid
signal and IDs, unknown IDs, and safe refusal of nonzero signals.
