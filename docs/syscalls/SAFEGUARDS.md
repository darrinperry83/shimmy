# Syscall safeguards and withheld work

This ledger distinguishes a tested, bounded Linux behavior from an isolated
experiment or an honest refusal. It is updated with each reviewed batch. A
successful dispatcher case, one guest probe, or an Arch login does not mean
full syscall compatibility.

| ARM64 number | Family | Published default | Safeguard / evidence | Remaining gate |
| ---: | --- | --- | --- | --- |
| 26–28 | `inotify` | Bounded file and directory events | Named create/delete, watched-file events, alias close, guest probes and private boot passed. | Coalesced/transient events, moves, overflow, inherited workers and broader close paths. |
| 35 | `unlinkat` flag validation | Existing handler | An isolated branch passed a focused guest probe and reached a private Arch login prompt, but automatic review interrupted the agent before its boot result, cleanup proof, and commit were complete. The branch was withheld and the scoped process check found no remaining guest. | Complete independent review and tests before merging; do not count this candidate as published. |
| 39 | `umount2` | Validated refusal for existing targets | Isolated ARM64 probe and private Arch login/D-Bus boot passed; no host unmount occurs. | Guest mount namespace, detach semantics and shutdown-path testing. |
| 77 | `tee` | `ENOSYS` | Inactive virtual-pipe core is tested. A separate opt-in socket-pipe guest branch passed tests and private boot but stays isolated due incomplete pipe semantics and a measured small-I/O slowdown. | A performant, coherent pipe identity and all fd/I/O paths. |
| 139 | `rt_sigreturn` | `ENOSYS` after this correction | No guest signal frames or saved register context exist. A raw ARM64 guest probe confirms the former false success is replaced by a clear refusal. | Construct and validate guest signal frames, handler entry, context restoration and mask restoration. |
| 149 | `setresgid` | Host-identity mode refuses explicit saved-GID changes | ARM64 no-mutation probe, getresgid regression probe, and private Arch login/D-Bus boot passed; virtual-root path is unchanged. | Implement coherent saved-GID semantics without mutating unsupported host credentials. |
| 151–152 | `setfsuid`, `setfsgid` | Query/same-ID subset; differing IDs refused | ARM64 probes and separate private Arch login/D-Bus boots passed. Neither handler changes host credentials. | Separate guest filesystem IDs, file access enforcement and complete Linux return semantics. |
| 171 | `adjtimex` | Read-only `modes=0` query | Independent Arch ARM64 UAPI probe and existing raw probe pass; no runtime change was needed. | Isolated guest clock state and adjustment semantics; do not mutate the Mac's shared clock. |
| 180–185 | POSIX mqueue | `ENOSYS` | Private store and readiness dependencies are tested; an in-process guest adapter was withheld when review found the backing resource boundary incomplete. | Broker-owned backing, guest fd lifecycle, permissions and poll/epoll. |
| 186 | SysV `msgget` | `ENOSYS` after safeguard | ARM64 probe and private Arch login/D-Bus boot passed; guest key lookup/creation no longer reaches Darwin host-global IPC. | #187–189 still accept raw host queue IDs; isolate the full queue family before claiming compatibility. |
| 217–219 | Keyring | `ENOSYS` | Private object and subscription models remain inactive; no real guest key material is accepted. | Authenticated guest lifecycle, Linux permissions, confidential transactional storage. |
| 261 | `prlimit64` | Existing bounded behavior | A candidate passed ARM64 probes but failed the private Arch D-Bus boot gate while a fresh baseline passed. Candidate remains isolated. | Reconcile reported and enforceable limits without the boot regression. |
| 277 | `seccomp` | `ENOSYS` for filter installation | `PR_SET/GET_NO_NEW_PRIVS` has tested monotonic state. A negative guest test showed a dispatcher-only filter cannot enforce the full guest execution boundary. | Enforced execution isolation before filter installation can be trusted. |
| 282 | `userfaultfd` | `ENOSYS` | A narrow opt-in fault/copy/wake experiment passed guest and private boot tests, then its runtime merge was withheld pending review of private host-resource access. | Isolated resource boundary, broader mapping and lifecycle semantics. |
| 293 | `rseq` | `ENOSYS` | A four-thread guest test confirms clean fallback. Registration success is withheld because preemption/migration aborts are not guaranteed. | Scheduler and signal abort semantics. |
| 424 | `pidfd_send_signal` | Signal-zero liveness subset | Guest probe and private boot passed; unknown/stale descriptors, unsupported payloads and nonzero delivery are refused. | Target-owned guest signal delivery with identity and handler semantics. |
| 425–427 | `io_uring` | `ENOSYS` | An opt-in NOP-ring branch passed guest and private boot tests but was withheld after private backing-resource access was found incomplete. | Broker or comprehensive resource boundary, then broader operations. |
| 429 | `move_mount` | Existing no-op success; correction withheld | Isolated `ENOSYS` candidate passed ARM64 probe and CLI build but failed the 120-second private Arch login/D-Bus gate; fresh published baseline passed. Exact guests and clones were cleaned. | Trace boot dependency and implement coherent guest mount state or a tested fallback before replacing the default. |
| 430 | `fsopen` | Existing synthetic socket; correction withheld | Isolated `ENOSYS` candidate passed ARM64 probe and CLI build but failed the 120-second private Arch login/D-Bus gate; exact guest and clone were cleaned. | Trace the systemd mount-context fallback, then implement coherent guest mount state before replacing the default. |

Each isolated branch and its detailed proof is recorded in `PROGRESS.md` and
the corresponding per-syscall document. Withheld branches are not counted as
implemented. The published `development` branch keeps the default refusal
where the contract cannot yet be upheld.
