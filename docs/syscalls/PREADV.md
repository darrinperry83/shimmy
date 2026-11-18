# `preadv` (#69)

The old handler passed the guest iovec pointer directly to Darwin `preadv`.
It did not check guest ownership of the vector array or destination ranges,
and did not validate vector count before narrowing it to `int`.

The bounded implementation reads the Linux ARM64 iovec array through checked
guest memory, accepts at most 1,024 vectors, and reads at most 1 MiB per call
from a readable ordinary regular file into a host buffer. It copies the bytes
back through Shimmy's checked guest write interface in vector order. A later
invalid destination returns the number of bytes already copied; an invalid
first destination returns `EFAULT`. It keeps the file's current position
unchanged and uses the full signed 64-bit offset in x3; x4 is irrelevant on
64-bit ARM64. Negative offset, oversized vector count or invalid aggregate
length return `EINVAL`. Pipes return `ESPIPE` and invalid/wrong-mode fds return
`EBADF`.

Linux [`fs/read_write.c`](https://codebrowser.dev/linux/linux/fs/read_write.c.html)
defines `preadv` using `pos_from_hilo` and `do_preadv`, with explicit-position
reads leaving the file description's position unchanged. The disposable ARM64
probe at `tests/run-preadv.py` covers two vectors, short EOF, invalid vector
array/destination, zero vectors, pipe and closed fd, and a sparse offset over
4 GiB. Run it after `./build.sh cli` with `--sysroot /path/to/arch/rootfs`.

This does not cover nonregular seekable devices, concurrent mutation of guest
buffers, or reads larger than 1 MiB in one syscall. A full boot regression
check remains necessary before integration.
