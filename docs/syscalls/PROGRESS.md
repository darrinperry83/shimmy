# Syscall compatibility progress

This file records bounded behavior that has been implemented and tested. A dispatch case or a successful boot is not proof of full Linux compatibility. The static inventory in `AUDIT.md` is a triage list and may lag later commits.

## Integration baseline

- Base: `3e47e28` (`development` as tracked on 2026-11-18).
- Integration checkout: an isolated clone under `task-3/shimmy-syscall-integration`. The active development checkout contains unrelated work and is not used for these edits.
- Each syscall gets a separate worktree and branch. Merge only after a focused semantic test and source review. Preserve the exact tested subset and unresolved Linux behavior in the commit and this ledger.

## Current batch

| Number | Syscall | Branch | State | Evidence / remaining work |
| ---: | --- | --- | --- | --- |
| 28 | `inotify_rm_watch` | `syscall/28-inotify-rm-watch` | In progress | Existing code returns success unconditionally; require truthful watch lifecycle or a defensible failure. |
| 77 | `tee` | `syscall/77-tee` | Blocked: virtual pipe dependency | Existing code returns `ENOSYS`. Darwin pipe fds cannot be peeked (`MSG_PEEK` gives `ENOTSOCK`; `pread` gives `ESPIPE`); a correct implementation needs virtual pipe queue and readiness across read/write/dup/fork/exec. No code change or compatibility claim. |
| 100 | `get_robust_list` | `syscall/100-get-robust-list` | In progress | Existing code returns `ENOSYS`; inspect `set_robust_list` and futex cleanup before claiming compatibility. |
| 452 | `fchmodat2` | `syscall/452-fchmodat2` | Merged at `1286320` | Invalid flags return `EINVAL` without chmod; NULL path returns `EFAULT`; empty path with `AT_FDCWD` uses guest cwd. `./build.sh cli` and disposable static ARM64 guest test passed. Symlink, `O_PATH`, ownership and races remain unverified. |

## Already merged before this batch

The development history already contains bounded implementations of `openat2`, `vmsplice`, `sched_getattr`, `sched_setattr`, `execveat`, and `move_pages`. Their exact scope is in their individual commits and tests; none implies whole-table compatibility.
