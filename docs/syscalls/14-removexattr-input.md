# `removexattr` (AArch64 syscall 14): checked guest strings

The published handler passed a raw guest pathname and xattr name to host
path and xattr APIs. The `removexattr` branch now copies both bounded strings
through checked guest memory before resolving the path or removing data.
The neighboring `lremovexattr` and `fremovexattr` branches are unchanged.

The disposable ARM64 probe checks NULL and unmapped pathname and name
pointers, then removes an attribute from a file in a temporary guest root.
The final host check confirms removal. `./build.sh cli` passed. A separate
private Arch boot reached a fresh login prompt with D-Bus broker and launcher
settled; the harness stopped its exact guest process.

This is an input-safety and ordinary-file subset. Linux xattr namespace,
permission and concurrency behavior remains unverified.
