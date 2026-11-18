# `munlockall` (ARM64 syscall 231): blocked

Linux `munlockall` clears locks on every mapping and clears future-lock state. Shimmy previously called Darwin `munlockall()` and ignored its result, reporting success. An isolated host probe on this Mac returned `ENOSYS` from both Darwin `mlockall(MCL_FUTURE)` and `munlockall()`, while individual `mlock` and `munlock` worked. Therefore a guest can lock a page with syscall 228 and the old #231 would falsely report that it had unlocked it.

The handler now returns `ENOSYS` rather than success. This is **not working `munlockall` compatibility**. A guest must use `munlock` on its known ranges or handle the unsupported call. A functional implementation needs a complete guest mapping/lock registry across `gmem_mmap`, SysV `shmat`, DRM and ALSA mappings, and their unmap/detach paths. Walking only the current gmem ledger is insufficient: #228 `mlock` can lock a guest-visible mapping that the ledger does not include. Unconditionally calling `munlock` on the whole host process would also affect shim-private mappings.

Run `./build.sh cli` and `python3 tests/run-munlockall.py`. The static ARM64 guest maps and locks a page, verifies #231 reports `ENOSYS`, then explicitly unlocks and unmaps that page. The test proves the false-success correction, not a functional unlock-all operation. A private systemd boot gate is needed before integration because a caller may handle this error differently from the former false success.

Primary source: [Linux `mm/mlock.c`, `SYSCALL_DEFINE0(munlockall)`](https://github.com/torvalds/linux/blob/master/mm/mlock.c).
