# ARM64 #78 `readlinkat`: checked guest path and result

The previous route passed a raw guest pathname through host path resolution
and wrote ordinary and synthetic link targets directly into a guest pointer.
It could fault inside the shim for invalid addresses and could not read a
pathname stored in executable guest memory. The route now snapshots a bounded
guest pathname, computes the link target in host memory, and uses checked
copyout for exactly the returned bytes. It never adds a trailing NUL.

Ordinary link targets are staged within `GPATH_MAX` bytes; synthetic targets
retain their prior text and rootfs-prefix stripping remains in host memory.
The disposable ARM64 probe checks an executable guest pathname, ordinary and
`/proc/self/exe` links, truncated output without NUL, host-root prefix
stripping before truncation, invalid input/output addresses, and size zero.
CLI build and private Arch login/D-Bus boot are
regression gates. Long targets beyond the host staging bound and broader
concurrent pathname races are not covered by this bounded correction.
