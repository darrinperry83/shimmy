# ARM64 `dup3` (#24) metadata commit order

The previous `dup3` path registered virtual DRM, sound and input-device
metadata for the target descriptor before validating arguments or successfully
duplicating the source fd. A rejected call could therefore make an unchanged
regular-file descriptor appear to be a virtual device. These metadata updates
now follow successful `dup2`. The existing inotify, route-netlink and pidfd
duplication handling remains in its prior order after successful duplication.

`tests/run-dup3-metadata.py` runs a disposable ARM64 guest. It opens a synthetic
input device and an ordinary file, checks that invalid flags and an invalid
source leave the ordinary file's Linux `fstat` type intact, exercises
`oldfd==newfd` and fd reuse, and confirms a successful device dup propagates
the device type and `O_CLOEXEC`. The existing inotify directory guest test
also passed.

The focused probe, inotify regression and CLI build passed. A private Arch
boot reached a fresh login prompt with D-Bus settled; the exact guest and
rootfs clone were cleaned. This increment does not settle all
descriptor ownership or metadata replacement cases when a valid `dup3`
overwrites an already tagged target.
