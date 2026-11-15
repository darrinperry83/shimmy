"""Synthetic ps listing guards isolated boot cleanup from unrelated guests."""

from pathlib import Path
from boot_process_scope import boot_processes


binary = Path("/tmp/shimmy-development/shim_cli").resolve()
root = Path("/tmp/shimmy-development/build/boot-root").resolve()
listing = "\n".join((
    f"  101 {binary} {root} /sbin/init --system",
    f"  102 {binary} {root} /usr/bin/dbus-broker --log 5",
    f"  103 {binary} {root} /usr/bin/dbus-broker-launch --scope system",
    f"  104 {binary} {root}-other /sbin/init --system",
    f"  105 {binary}-other {root} /sbin/init --system",
    "  106 ./shim_cli build/boot-root /sbin/init --system",
    "  107 /bin/sh -c shim_cli boot-root",
))
assert boot_processes(listing, binary, root) == [
    (101, "/sbin/init --system"),
    (102, "/usr/bin/dbus-broker --log 5"),
    (103, "/usr/bin/dbus-broker-launch --scope system"),
]
print("PASS exact binary and root scoped process selection")
