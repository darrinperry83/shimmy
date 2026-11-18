# ARM64 #74 `signalfd4`: checked guest mask input

The existing backend supports a small set of Linux standard signals through
a host socket pair. Its syscall route previously passed a raw guest mask
pointer; the backend dereferenced it even while formatting diagnostics. The
route now checks the 8-byte ARM64 kernel sigset size, copies the mask from
guest memory into host memory, and calls the existing backend with that local
copy. An invalid guest address returns `EFAULT` before backend state changes.

This preserves the backend's supported-signal subset. It does not implement
full Linux signal delivery. During the ARM64 probe, a separate close-lifecycle
gap was observed: the signalfd read descriptor is marked shim-reserved, and
the core `close` route returns success early for reserved descriptors. A guest
close therefore does not retire this signalfd. That issue requires a separate
fd-ownership change and is not hidden by this mask correction.

Proof: `./build.sh cli`; `python3 tests/run-signalfd4-mask.py` checks invalid
mask pointers, the kernel sigset size, valid create and mask update, and
unsupported flags in a disposable ARM64 guest. The existing host
`test_signalfd_shim.c` passed with standalone reservation stubs, covering
readiness and 128-byte records. A private Arch login/D-Bus boot smoke checks
startup compatibility, not full signal or descriptor-lifecycle parity.
