# `getcwd` (#17)

The existing handler forms the guest's current directory from `g_cwd` and
returns its NUL-terminated byte length, but used a direct `memcpy` through the
guest pointer. That did not check whether the requested output belonged to a
writable guest mapping.

The bounded ARM64 handler keeps its existing null-pointer `EFAULT` and
too-small-buffer `ERANGE` behavior, then uses checked guest copyout for exactly
`strlen(g_cwd) + 1` bytes. An unmapped or executable read-only destination now
returns `EFAULT`. The copyout ledger checks the whole output before writing
and holds its lock across the write; it does not promise protection from
untracked host-side changes to the VM mapping.

`tests/run-getcwd-memory.py` builds a disposable ARM64 guest that changes to
`/tmp`, verifies the returned length and NUL termination in stack and static
buffers, and checks `ERANGE`, invalid, null, and executable read-only outputs.
The isolated CLI build and probe passed. A private Arch boot reached a fresh
login prompt with D-Bus broker and launcher settled; its exact guest and clone
were cleaned.
