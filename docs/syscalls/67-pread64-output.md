# ARM64 syscall 67: checked `pread64` output

The old route passed a guest output pointer directly to Darwin `pread`.
That bypassed Shimmy's guest range admission and could ask the host to write
to an unmapped or read-only executable/JIT region. The bounded route now
supports readable regular files, nonnegative signed ARM64 offsets, and up to
1 MiB of output. It checks that the guest range is writable before a host
read, reads into a host-owned buffer once, and copies out only the actual
byte count through the checked guest-memory API. Larger requests return
`EOPNOTSUPP`; unsupported descriptor types return `ESPIPE`.

`pread64` does not change the file description's current offset, including
on a short EOF read. A guest can still unmap or protect its output between
preflight and copyout; a late failure then returns `EFAULT` after the host
read. That race is outside this bounded guarantee. This does not establish
full Linux behavior for block devices or requests over 1 MiB.

Proof: `./build.sh cli` and `python3 tests/run-pread64-checked.py`. The ARM64
guest tests a nonzero offset, unchanged file position, stack and static
writable outputs, partial EOF, read-only executable/JIT and invalid output
rejection, size/offset bounds, an invalid fd, and zero count. A private Arch
An isolated private Arch boot reached a fresh login prompt with D-Bus broker
and launcher settled. The exact guest and clone were cleaned. This boot smoke
does not establish full block-device or long-running application behavior.
