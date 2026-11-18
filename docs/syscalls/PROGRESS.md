# Syscall compatibility progress

This file records bounded behavior that has been implemented and tested. A dispatch case or a successful boot is not proof of full Linux compatibility. The static inventory in `AUDIT.md` is a triage list and may lag later commits.

## Integration baseline

- Base: `3e47e28` (`development` as tracked on 2026-11-18).
- Integration checkout: an isolated clone under `task-3/shimmy-syscall-integration`. The active development checkout contains unrelated work and is not used for these edits.
- Each syscall gets a separate worktree and branch. Merge only after a focused semantic test and source review. Preserve the exact tested subset and unresolved Linux behavior in the commit and this ledger.

## Current batch

| Number | Syscall | Branch | State | Evidence / remaining work |
| ---: | --- | --- | --- | --- |
| 28 | `inotify_rm_watch` | `syscall/28-inotify-rm-watch` | Merged at `77f4b7e`; partial | Watch state and `IN_IGNORED` removal are tested in host and disposable ARM64 guest probes. Filesystem-change events are still absent; reserved fd lifecycle remains limited. |
| 77 | `tee` | `syscall/77-tee` | Blocked: virtual pipe dependency | Existing code returns `ENOSYS`. Darwin pipe fds cannot be peeked (`MSG_PEEK` gives `ENOTSOCK`; `pread` gives `ESPIPE`); a correct implementation needs virtual pipe queue and readiness across read/write/dup/fork/exec. No code change or compatibility claim. |
| 100 | `get_robust_list` | `syscall/100-get-robust-list` | Merged at `23317ab`; partial | Per-thread registration/query, fork reset, `ESRCH`/`EFAULT` pass disposable ARM64 ABI and thread tests. Owner-death futex cleanup and cross-process queries remain absent. |
| 452 | `fchmodat2` | `syscall/452-fchmodat2` | Merged at `1286320` | Invalid flags return `EINVAL` without chmod; NULL path returns `EFAULT`; empty path with `AT_FDCWD` uses guest cwd. `./build.sh cli` and disposable static ARM64 guest test passed. Symlink, `O_PATH`, ownership and races remain unverified. |
| 272 | `kcmp` | `syscall/272-kcmp` | Merged; bounded subset | Current-process self resource classes and same valid fd comparison pass a disposable ARM64 guest test. Distinct fds, cross-process comparisons, and epoll target identity remain unsupported. |
| 286 | `preadv2` | `syscall/286-preadv2` | Merged; bounded subset | ARM64 x3 full offset, x4 ignored, current-position `-1`, and unsupported flag rejection pass a disposable guest test, including a read above 4 GiB. Nonzero RWF flags remain unsupported. |
| 287 | `pwritev2` | `syscall/287-pwritev2` | Merged; bounded subset | ARM64 x3 full offset, x4 ignored, current-position `-1`, and no-write rejection of unsupported flags pass a disposable guest test, including a sparse write above 4 GiB. Nonzero RWF flags remain unsupported. |
| 33 | `mknodat` | `syscall/33-mknodat` | Merged at `b390262`; partial | Regular-file and FIFO creation plus invalid-type errno distinctions pass a disposable ARM64 guest test. Device nodes and sockets remain unsupported; ownership, ACL, and path races unverified. |
| 288 | `pkey_mprotect` | `syscall/288-pkey-mprotect` | Merged at `1de7cb2`; bounded subset | Default key 0 and legacy key -1 route through guest memory protection; invalid key, alignment, and protection checks pass. Hardware keys, allocation, and MTE remain unsupported. |
| 438 | `pidfd_getfd` | `syscall/438-pidfd-getfd` | Merged at `f6be319`; bounded subset | A self-process kqueue pidfd can duplicate a regular guest-root file with shared offset and `CLOEXEC`; guest test passes. Cross-process, socket pidfd, synthetic and nonregular fd transfer remain unsupported. |
| 454 | `futex_wake` | `syscall/454-futex-wake` | Merged at `d74578d`; bounded subset | Private aligned U32 wake routes through the existing futex queue. Flags, mask, count, and invalid addresses are checked in a disposable ARM64 guest test. Shared and NUMA futexes remain unsupported. |
| 464 | `getxattrat` | `syscall/464-getxattrat` | Merged at `dc82da6`; bounded subset | Guest-root path and dirfd xattr reads, size queries, versioned arguments, flags, and bad-address handling pass a disposable ARM64 guest test. `AT_EMPTY_PATH` fd reads remain unsupported. |
| 465 | `listxattrat` | `syscall/465-listxattrat` | Merged at `8abbe0f`; bounded subset | Guest-root path, dirfd, and ordinary guest-root fd empty-path xattr lists, size queries, flags, and symlink behavior pass a disposable ARM64 guest test. Broader synthetic fd and filesystem behavior remain unverified. |
| 455 | `futex_wait` | `syscall/455-futex-wait` | Merged at `aa129b3`; bounded subset | Private aligned U32 wait with mask and absolute monotonic/realtime timeout passes a disposable ARM64 guest test. Shared, NUMA, and broader futex2 modes remain unsupported. |
| 463 | `setxattrat` | `syscall/463-setxattrat` | Merged at `0a9bc8e`; bounded subset | Guest-root path and dirfd writes, CREATE/REPLACE, versioned arguments, bounds, and bad-address rejection pass a disposable ARM64 guest test. Fd empty-path writes remain unsupported. |
| 466 | `removexattrat` | `syscall/466-removexattrat` | Merged at `27511e3`; bounded subset | Guest-root path and dirfd removals, flags, missing attribute, and bad-address rejection pass a disposable ARM64 guest test. Fd empty-path removals remain unsupported. |
| 451 | `cachestat` | `syscall/451-cachestat` | Blocked: no cache-state model | Existing `ENOSYS` is preserved. Darwin descriptors do not supply Linux per-file cached, dirty, writeback, and eviction counters; see `CACHESTAT.md`. |
| 456 | `futex_requeue` | `syscall/456-futex-requeue` | Merged at `53d2b9e`; bounded subset | Private U32, zero-requeue wake with source-value comparison passes a disposable ARM64 guest test. Moving waiters needs queue redesign and remains unsupported. |
| 447 | `memfd_secret` | `syscall/447-memfd-secret` | Blocked: secret-memory isolation | Existing `ENOSYS` is preserved. Ordinary Darwin mappings cannot promise Linux direct-map exclusion, dump exclusion, and zero-on-release; see `MEMFD_SECRET.md`. |
| 449 | `futex_waitv` | `syscall/449-futex-waitv` | Merged at `8aae551`; bounded subset | One private aligned U32 waiter, with mismatch/timeout and real wake, passes a disposable ARM64 guest test. Atomic waits on multiple addresses need queue redesign. |

## Already merged before this batch

The development history already contains bounded implementations of `openat2`, `vmsplice`, `sched_getattr`, `sched_setattr`, `execveat`, and `move_pages`. Their exact scope is in their individual commits and tests; none implies whole-table compatibility.

## Batch integration proof

At the isolated integration head, `./build.sh cli`, `tests/run-inotify-rm-watch.sh`, `python3 tests/run-get-robust-list.py`, and `python3 tests/run-fchmodat2.py --sysroot <installed-Arch-root>` all passed on 2026-11-18. These are focused tests, not a systemd/desktop regression run.

After merging the second batch, `./build.sh cli` and `python3 tests/run-kcmp.py`, `python3 tests/run-preadv2.py`, and `python3 tests/run-pwritev2.py` (each with `--sysroot <installed-Arch-root>`) passed together on 2026-11-18. The native ARM64 offset interpretation was checked against Linux `fs/read_write.c` `pos_from_hilo` and both vectored I/O tests exercise x3/x4 separately.

After merging the third batch, `./build.sh cli` and `python3 tests/run-mknodat.py`, `python3 tests/run-pkey-mprotect.py`, and `python3 tests/run-pidfd-getfd.py` (each with `--sysroot <installed-Arch-root>`) passed together on 2026-11-18. No live GUI or installed rootfs was modified.

After merging the fourth batch, `./build.sh cli`, `python3 tests/run-futex-wake2.py`, `python3 tests/run-getxattrat.py --sysroot <installed-Arch-root>`, and `python3 tests/run-listxattrat.py --sysroot <installed-Arch-root>` passed together on 2026-11-18. The `getxattrat`/`listxattrat` switch conflict was resolved by retaining both syscall cases. No live GUI or installed rootfs was modified.

After merging the fifth batch, `./build.sh cli` and all six focused tests for syscall numbers 454, 455, and 463–466 passed together on 2026-11-18. `setxattrat` and `removexattrat` run against disposable guest roots; the installed Arch root is used only as a read-only compiler sysroot. This is not a live systemd or desktop regression test.

After merging the sixth batch, `./build.sh cli`, `python3 tests/run-futex-requeue2.py`, `python3 tests/run-futex-wait2.py`, and `python3 tests/run-futex-wake2.py` passed together on 2026-11-18. `cachestat` received a blocker note and no behavior change. Real futex waiter relocation remains unimplemented.

After merging the seventh batch, `./build.sh cli` and `python3 tests/run-futex-waitv.py`, `python3 tests/run-futex-requeue2.py`, `python3 tests/run-futex-wait2.py`, and `python3 tests/run-futex-wake2.py` passed together on 2026-11-18. `memfd_secret` received a security-contract blocker note and no behavior change.
