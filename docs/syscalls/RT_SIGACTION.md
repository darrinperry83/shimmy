# `rt_sigaction` (134): checked guest memory subset

The native ARM64 ABI requires an eight-byte sigset size and a 32-byte action
structure. This increment stages the new action through checked guest memory,
checks and copies the old action before changing the recorded disposition, and
preserves the existing parent-death custom-handler refusal. Invalid pointers
return `EFAULT` without changing the recorded action. Input and output may
alias because the input is staged first.

This remains a **recorded disposition**, not full Linux signal delivery.
Shimmy does not install guest handlers or construct guest signal frames; broader
handler execution, masks and realtime behavior remain unsupported. This change
does not claim that installing a handler makes it callable.

Proof: `./build.sh cli` and `python3 tests/run-rt-sigaction.py` exercise the
disposable ARM64 ABI, error paths, unchanged state after invalid output, and
readback after ordinary and aliased writes.
