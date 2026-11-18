# ARM64 syscall 66: `writev` byte count under trace

The ordinary `writev` route uses Darwin's compatible `iovec` ABI and returns
the host byte count. When `RDWRDUMP` was enabled for a low file descriptor,
the diagnostic formatter decremented that same return variable while walking
the vectors. A successful five-byte write could therefore return zero to the
Linux application. This could cause retry loops or duplicate output while
debugging an application.

The formatter now tracks a separate remaining count, preserving the syscall
result. `RDWRDUMPFILE` optionally directs that diagnostic to a caller-owned
file; without it, the original `/tmp/rd_wrdump.txt` default remains. This
allows a disposable test to enable tracing without touching an existing user
diagnostic file.

Verification: `./build.sh cli` and `python3 tests/run-writev-return.py`.
The ARM64 guest writes two vectors to stdout and checks the exact byte count
with tracing off and on. The runner verifies the trace contents in its private
temporary directory. A private Arch boot smoke is a separate gate.
That isolated boot reached a fresh login prompt with D-Bus broker and
launcher settled; it does not validate every `writev` memory region.

Separate unresolved gap found during this probe: when the iovec payloads were
in static guest ELF data, Darwin `writev` returned `EFAULT`; stack-backed
payloads worked. The runtime currently forwards the guest iovec and payload
addresses directly. Bridging static/JIT-backed guest memory into host-safe
buffers needs its own design and tests across regular files, pipes and sockets.
This change fixes only the diagnostic return value, not that memory path.
