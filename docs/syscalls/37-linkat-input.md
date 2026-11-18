# `linkat` (AArch64 syscall 37): checked guest pathnames

The published handler already validates supported flags and distinguishes
symlink-follow behavior. It still handed raw guest addresses to the path
resolver. Both names are now copied through checked guest memory before
resolution or host hard-link creation. An invalid destination therefore
returns `EFAULT` without creating a link.

The disposable ARM64 probe checks NULL and unmapped path pointers and
creates absolute and dirfd-relative hard links. It verifies target contents
and inode identity inside a temporary guest root. `./build.sh cli` passed.
A separate private Arch boot reached a fresh login prompt with the D-Bus
broker and launcher settled; the harness stopped its exact guest process.

This establishes a bounded input and ordinary-file subset. Cross-filesystem,
permission, concurrent path replacement, and `AT_EMPTY_PATH` behavior remain
outside this probe; the existing `AT_EMPTY_PATH` refusal is unchanged.
