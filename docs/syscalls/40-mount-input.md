# `mount` (AArch64 syscall 40): checked guest strings

The current synthetic mount handler directly read guest target and
filesystem-type pointers. It now copies the required target and optional
filesystem type through checked bounded guest memory before making its
existing decision. Invalid pointers return `EFAULT` rather than faulting
the shim or receiving a synthetic success.

The disposable ARM64 probe checks NULL and unmapped target pointers, an
unmapped filesystem-type pointer, and the unchanged synthetic call for
`tmpfs` on `/tmp`. `./build.sh cli` passed. A separate private Arch boot
reached a fresh login prompt with D-Bus broker and launcher settled; the
harness stopped its exact guest process.

This is an input-safety correction only. The mount handler still lacks a
guest mount table and returns success for broad requests without changing
mount state. Source and data arguments are also outside this focused check.
The ledger therefore remains `placeholder`, not Linux-compatible.
