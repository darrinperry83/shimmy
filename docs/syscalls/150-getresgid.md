# `getresgid` (AArch64 150)

The handler now writes three 32-bit guest GIDs through checked guest-memory
copyout. All three Linux output pointers are required; null or inaccessible
addresses return `EFAULT` instead of dereferencing a guest pointer in the Mac
process.

With `RDUID0`, the result comes from Shimmy's existing real, effective, and
saved guest GID slots. The ARM64 probe changes those slots independently with
`setresgid` and reads them back. Without `RDUID0`, real and effective GIDs come
from the current host process and the saved GID comes from Darwin's own
`KERN_PROC_PID` credential record. This avoids the old assumption that saved
always equals effective. If Darwin cannot provide that record, the call fails
instead of inventing a saved GID.

`./build.sh cli` and `python3 tests/run-getresgid.py` pass for ordinary and
virtual-root disposable guests, including invalid output pointers. This does
not make Shimmy's whole credential model Linux compatible: guest filesystem
permission checks, supplementary groups, namespaces, and the separate fs GID
remain incomplete.
