# ARM64 #196 `shmat`: host IPC attachment safeguard

The previous route passed an arbitrary guest numeric segment ID and requested
address straight to Darwin `shmat`. Because Darwin SysV shared memory uses a
host-wide namespace, a same-user segment could be attached, exposing host
memory to a guest. A valid guest ID registry and attachment lifetime model do
not yet exist. #196 now returns `ENOSYS` before calling host IPC.

This deliberately disables `shmat`, including any Qt `QSharedMemory` path.
It does not make the SysV shared-memory family compatible: `shmctl`
(#195) and `shmdt` (#197) now also refuse host IPC. A future
implementation needs per-instance opaque IDs, permission checks, mapped-page
accounting, fork/exec ownership and final cleanup.

`tests/run-shmat-safeguard.py` compiles a disposable ARM64 guest that probes
only invalid IDs, which cannot attach host shared memory even if the safeguard
regresses. The unconditional source-level return covers all IDs. Passing this
probe and the private Arch boot gate demonstrates refusal and boot stability,
not Linux shared-memory compatibility.
