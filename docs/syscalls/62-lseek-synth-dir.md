# ARM64 #62 `lseek`: synthetic directory positions

`getdents64` serves `/proc`, `/sys`, `/dev`, and synthetic cgroup directories
from an entry table with a per-fd cursor. The previous `lseek` route moved
only the backing host directory. A guest rewind therefore left its synthetic
cursor at EOF, while `SEEK_CUR` returned the host position rather than the
cookie emitted in `d_off`.

For recognized synthetic directory fds, `lseek` now uses the same cursor.
It accepts `SEEK_SET` to zero or a cookie emitted by `getdents64`, and
`SEEK_CUR` relative to that guest position. Other seeks return `EINVAL` rather
than suggesting that moving the backing directory changed the guest stream.
Ordinary file seeking still uses the host `lseek` path.

`tests/run-lseek-synth-dir.py` exercises `/proc` enumeration, rewind, a
returned `d_off` cookie, an invalid cookie, and ordinary file offsets in a
disposable ARM64 guest. The private Arch boot gate checks login and settled
D-Bus. This correction does not provide a general stable cookie model for
all dynamic synthetic tables across concurrent changes.
