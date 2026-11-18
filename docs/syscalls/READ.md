# `read` (#63)

The old handler passed a guest output pointer directly to Darwin `read`.
Guest ELF or executable JIT mappings can require Shimmy's checked guest-memory
bridge, and an invalid destination could consume bytes from a pipe or socket
before the shim noticed the error.

The bounded ARM64 handler checks that the destination is guest-writable before
calling the host. It reads once into a host-owned buffer, then copies exactly
the returned prefix into guest memory. Zero-length reads retain the host's
descriptor result without inspecting the pointer. The existing optional
`RDDESERDBG` byte dump now reads the staged buffer.

To avoid truncating an unknown datagram, requests above 1 MiB return
`EOPNOTSUPP` before reading. This is a compatibility limit, including different
error precedence for oversized requests. A guest mapping could also change
after the preflight but before copyout; a late copyout failure returns `EFAULT`
after the host may have consumed bytes. This increment does not provide
atomic guest-memory mapping ownership.

`tests/run-read-memory.py` builds a disposable raw ARM64 guest probe. It covers
stack and static destinations, invalid and executable read-only destinations
without pipe consumption, oversized refusal, a zero-length read, a short read
at EOF, and an invalid descriptor. The existing `readv` probe is a neighboring
regression check. The isolated private Arch boot reached a fresh login prompt
with D-Bus broker and launcher settled; its exact guest and clone were cleaned.
