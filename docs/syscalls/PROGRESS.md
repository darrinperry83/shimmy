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

## Already merged before this batch

The development history already contains bounded implementations of `openat2`, `vmsplice`, `sched_getattr`, `sched_setattr`, `execveat`, and `move_pages`. Their exact scope is in their individual commits and tests; none implies whole-table compatibility.

## Batch integration proof

At the isolated integration head, `./build.sh cli`, `tests/run-inotify-rm-watch.sh`, `python3 tests/run-get-robust-list.py`, and `python3 tests/run-fchmodat2.py --sysroot <installed-Arch-root>` all passed on 2026-11-18. These are focused tests, not a systemd/desktop regression run.

After merging the second batch, `./build.sh cli` and `python3 tests/run-kcmp.py`, `python3 tests/run-preadv2.py`, and `python3 tests/run-pwritev2.py` (each with `--sysroot <installed-Arch-root>`) passed together on 2026-11-18. The native ARM64 offset interpretation was checked against Linux `fs/read_write.c` `pos_from_hilo` and both vectored I/O tests exercise x3/x4 separately.
