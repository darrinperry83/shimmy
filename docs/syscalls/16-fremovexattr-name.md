# ARM64 `fremovexattr` (#16) checked name

The guest xattr name is copied into a bounded 256-byte host buffer before the
descriptor-based removal. A bad guest name pointer returns `EFAULT` without
mutating the xattr. The existing descriptor behavior is unchanged; this fix
does not establish complete ownership or namespace isolation for every guest
descriptor type.

`tests/run-fremovexattr-name.py` creates a disposable file with a single xattr
and runs a raw ARM64 guest. The guest opens the file, confirms the attribute,
passes an invalid name and verifies the attribute remains, then removes it
using a name stored in executable guest memory. An invalid descriptor still
returns `EBADF`.

The focused guest probe and CLI build passed. A private Arch boot reached a
fresh login prompt with D-Bus broker and launcher settled; the exact guest
and rootfs clone were cleaned.
