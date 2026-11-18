# ARM64 #133 `rt_sigsuspend`

The eight-byte guest signal mask is copied through the checked guest-memory
path before the virtual signal wait changes its mask. A null or inaccessible
mask returns `EFAULT`; a non-eight-byte size returns `EINVAL` when the pointer
is non-null. The signal-wait implementation no longer dereferences a guest
pointer directly.

`tests/guest_sigsuspend_child.c` checks invalid size, null and non-null invalid
addresses, then waits for a guest child exit and verifies `EINTR`, elapsed
time, restored mask and child exit status. It was compiled as an AArch64
static PIE and run in a disposable minimal guest root with `shim_cli`.

This is a memory-safety correction, not complete Linux signal semantics. The
existing virtual mask is process-wide, and the shim does not synthesize guest
handler frames. The child-exit wake is the tested positive case.
