# `tkill` (native ARM64 syscall 130): guest thread target check

Linux's [signal implementation](https://github.com/torvalds/linux/blob/master/kernel/signal.c)
rejects a nonpositive thread ID with `EINVAL`, and signal zero checks target
existence without delivery. Shimmy's shared tkill/tgkill path previously
returned success for signal zero before checking any target; a nonpositive
target with a nonzero signal could instead be routed to Shimmy's own host PID.

The bounded change checks a positive, known **guest** thread ID for `tkill`
before the existing signal path. An unknown ID returns `ESRCH`. It does not
forward a guest ID to Darwin or change `tgkill` (131). Actual positive signal
delivery retains the existing process-directed host behavior; this is a
target-validation fix, not Linux thread-directed signal parity.

`python3 tests/run-tkill-target.py` compiles a freestanding ARM64 guest and
checks the current guest TID with signal zero, nonpositive and unknown TIDs,
an unknown positive-signal target, and an invalid signal. `./build.sh cli`
and the guest probe passed in the isolated worktree. A private Arch boot using
the current `task-3` fixture reached a fresh login prompt with the D-Bus
broker and launcher settled. Its scoped guest and exact clone were cleaned.
