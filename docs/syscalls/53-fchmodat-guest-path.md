# `fchmodat` (AArch64 syscall 53): checked guest pathname

The original three-argument Linux `fchmodat` handler handed the raw guest
pathname pointer to the shared resolver. A NULL pointer already failed, but
an unmapped nonzero address could be dereferenced by host-side string code
before the syscall returned `EFAULT`. The handler now snapshots a bounded
pathname using the existing checked guest-memory helper before resolution.

The disposable ARM64 probe confirms that NULL and unmapped pointers return
`EFAULT`, while a valid pathname changes the mode of a file inside a
temporary guest root. It also checks that x3 is ignored because syscall 53
has only three Linux arguments; flags belong to `fchmodat2` (#452).

This checks pointer handling and ordinary-file behavior. Guest ownership,
ACLs, symlink race behavior, and full Linux permission semantics on
Darwin-backed files remain outside this focused change.
