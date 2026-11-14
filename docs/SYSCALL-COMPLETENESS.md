# Linux aarch64 Syscall ABI Completeness — Shim Audit

Audit date: 2026-11-14
Shim tree audited: `/Users/dperry/Documents/Codex/2026-11-07/task/hyprland-stage/metal-wayland-next/shim-metal-clear`
Scope: syscall **number** coverage + `/proc`/`/sys`/`/dev` synthesis. This is an audit only — no shim
source was modified.

Dispatch topology (where each syscall is answered):

| Route | Location | Notes |
|---|---|---|
| `route_syscall()` | `src/shim.c:187` | `GPU_SYSCALL`/`HOSTCALL_SYSCALL` → `net_owns_syscall()` → `linux_syscall()` |
| `clone(220)` | `src/shim.c:1027` `do_clone()` | needs the full register file; excluded from the fastcall thunk |
| sockets/epoll/poll/pipe/eventfd | `src/net.c:1887` `net_owns_syscall()` | 26 syscalls (see table) |
| core syscalls | `src/syscalls.c:2254` `linux_syscall()` | 66 `case` labels inline |
| long tail | `src/syscalls_ext.c:694` `linux_syscall_ext()` | everything else; `default:` → `-LX_ENOSYS` at `syscalls_ext.c:1499` |

The reference table is Linux `include/uapi/asm-generic/unistd.h` (the table arm64 uses). On arm64 the
number space through v6.10 is **318 defined syscalls**: `0..243` (244), `260..294` (35), `424..462` (39).
`244..259` (arch-reserved, arm64 uses none) and `295..402` (unassigned) and `403..423` (`*_time64`, which
**do not exist on arm64**) are not part of the ABI. The shim's `403/406/407/412/413/414` handlers are
therefore inert on arm64 (harmless dead code; the "~450" figure counts the whole number space, not the
318 defined entries).

---

## 1. Coverage summary

| Status | Count | % of 318 |
|---|---|---|
| ✅ **Implemented** (real/faithful semantics) | **163** | 51.3% |
| 🟡 **Partial** (stub / approximation / no-op / shadow / inert fd) | **46** | 14.5% |
| ⛔ **Missing** (returns `ENOSYS`) | **71** | 22.3% |
| 🚫 **Refused** (returns `EPERM`/`EOPNOTSUPP` — deliberately "not permitted") | **38** | 11.9% |
| **Not functional (Missing + Refused)** | **109** | 34.3% |

> "Implemented" includes a few with documented edge gaps (e.g. `mremap` lacks `MREMAP_DONTUNMAP`,
> `getrlimit/setrlimit` report "unlimited" for indices 10–15). "Partial" includes no-op/approximate
> handlers that *succeed* but do not provide the real semantic (inotify never fires, signals are
> shadow-only, `madvise`/`fadvise64`/`readahead` are ignored).

### The structural limitations (bigger than any single syscall)

1. **Signals are never delivered to guest code.** The guest runs under a Mach exception handler; the
   shim records signal dispositions but never builds/enters a signal frame. Consequently the whole
   `rt_sig*` family (`sigaction/sigprocmask/sigpending/sigsuspend/sigtimedwait/sigaltstack/sigreturn`)
   is **shadow-only**, and anything built on signal delivery (`ptrace`, `seccomp`, userfaultfd,
   `PROT_*`-fault handlers) cannot be real.
2. **No pid/process namespace.** `fork`/`clone`/`vfork` children are real host children; guest pids and
   tids are shim-invented. `pidfd_*` cannot exist until there is a guest pid table.
3. **`/proc`, `/sys`, `/dev` have no synthesized *directory* enumeration.** Only specific **leaf files**
   are served (via `openat`/`readlinkat`/`statx`/`newfstatat`); `getdents64` on any of these trees reads
   the host rootfs, which ships empty (see §4).

---

## 2. Full syscall-by-syscall table

Legend: ✅ implemented · 🟡 partial · ⛔ ENOSYS · 🚫 EPERM/EOPNOTSUPP refusal · `syscalls.c`=core,
`ext`=`syscalls_ext.c`, `net`=`net.c`, `shim.c`=trap handler.

### 0–99

| nr | name | status | handler (file:line) | note |
|---|---|---|---|---|
| 0–4 | io_setup/destroy/submit/cancel/getevents | ⛔ | ext:704 | aio; whole family ENOSYS |
| 5 | setxattr | ✅ | ext:705 | |
| 6 | lsetxattr | ✅ | ext:705 | |
| 7 | fsetxattr | ✅ | ext:705 | |
| 8 | getxattr | ✅ | ext:716 | |
| 9 | lgetxattr | ✅ | ext:716 | |
| 10 | fgetxattr | ✅ | ext:716 | |
| 11 | listxattr | ✅ | ext:727 | |
| 12 | llistxattr | ✅ | ext:727 | |
| 13 | flistxattr | ✅ | ext:727 | |
| 14 | removexattr | ✅ | ext:737 | |
| 15 | lremovexattr | ✅ | ext:737 | |
| 16 | fremovexattr | ✅ | ext:737 | |
| 17 | getcwd | ✅ | syscalls.c:2262 | tracked `g_cwd` |
| 18 | lookup_dcookie | ⛔ | ext:745 | also `sys_ni_syscall` on Linux |
| 19 | eventfd2 | ✅ | net:1924 | self-connected datagram socket |
| 20 | epoll_create1 | ✅ | net:1927 | kqueue-backed |
| 21 | epoll_ctl | ✅ | net:1929 | kqueue-backed |
| 22 | epoll_pwait | ✅ | net:1932 | sigmask ignored |
| 23 | dup | ✅ | ext:746 | propagates drm/snd/evdev registry |
| 24 | dup3 | ✅ | ext:755 | |
| 25 | fcntl | ✅ | syscalls.c:2272 | `do_fcntl` |
| 26 | inotify_init1 | 🟡 | ext:762 | inert fd (pipe), never fires |
| 27 | inotify_add_watch | 🟡 | ext:773 | allocates wd, never fires |
| 28 | inotify_rm_watch | 🟡 | ext:777 | returns 0 |
| 29 | ioctl | ✅ | syscalls.c:2275 | large `do_ioctl` (tty/pty/drm/snd/evdev/…) |
| 30 | ioprio_set | 🟡 | ext:778 | no-op |
| 31 | ioprio_get | 🟡 | ext:778 | no-op |
| 32 | flock | ✅ | syscalls.c:2286 | |
| 33 | mknodat | 🟡 | ext:779 | FIFO/regular only; device nodes EPERM |
| 34 | mkdirat | ✅ | syscalls.c:2289 | |
| 35 | unlinkat | ✅ | syscalls.c:2295 | |
| 36 | symlinkat | ✅ | ext:797 | |
| 37 | linkat | ✅ | syscalls.c:2310 | |
| 38 | renameat | ✅ | syscalls.c:2320 | |
| 39 | umount2 | 🚫 | ext:804 | EPERM |
| 40 | mount | 🚫 | ext:805 | EPERM |
| 41 | pivot_root | 🚫 | ext:806 | EPERM |
| 42 | nfsservctl | ⛔ | ext:807 | also `sys_ni_syscall` on Linux |
| 43 | statfs | ✅ | syscalls.c:2330 | |
| 44 | fstatfs | ✅ | syscalls.c:2338 | |
| 45 | truncate | ✅ | ext:808 | |
| 46 | ftruncate | ✅ | syscalls.c:2349 | enforces memfd seals |
| 47 | fallocate | 🟡 | ext:812 | KEEP_SIZE + PUNCH_HOLE only |
| 48 | faccessat | ✅ | syscalls.c:2365 | no `AT_EACCESS` (see faccessat2) |
| 49 | chdir | ✅ | syscalls.c:2386 | tracks `g_cwd` |
| 50 | fchdir | ✅ | ext:813 | |
| 51 | chroot | 🚫 | ext:814 | EPERM |
| 52 | fchmod | ✅ | ext:815 | |
| 53 | fchmodat | ✅ | ext:816 | |
| 54 | fchownat | ✅ | ext:822 | no-op under RDUID0 |
| 55 | fchown | ✅ | ext:830 | |
| 56 | openat | ✅ | syscalls.c:2410 | `/proc`/`/sys`/`/dev` synthesis entry |
| 57 | close | ✅ | syscalls.c:2579 | registry cleanup |
| 58 | vhangup | 🚫 | ext:831 | EPERM |
| 59 | pipe2 | ✅ | net:1936 | |
| 60 | quotactl | 🚫 | ext:833 | EPERM |
| 61 | getdents64 | ✅ | syscalls.c:2601 | host readdir only (see §4) |
| 62 | lseek | ✅ | syscalls.c:2604 | |
| 63 | read | ✅ | syscalls.c:2612 | passthrough |
| 64 | write | ✅ | syscalls.c:2626 | passthrough |
| 65 | readv | ✅ | syscalls.c:2650 | |
| 66 | writev | ✅ | syscalls.c:2651 | |
| 67 | pread64 | ✅ | syscalls.c:2654 | |
| 68 | pwrite64 | ✅ | syscalls.c:2655 | |
| 69 | preadv | ✅ | ext:836 | |
| 70 | pwritev | ✅ | ext:840 | |
| 71 | sendfile | ✅ | ext:844 | bounce-buffer copy (not zero-copy) |
| 72 | pselect6 | ✅ | net:1939 | |
| 73 | ppoll | ✅ | net:1942 | |
| 74 | signalfd4 | ✅ | ext:851 | `signalfd_shim.c` |
| 75 | vmsplice | ⛔ | ext:852 | |
| 76 | splice | ✅ | ext:853 | bounce-buffer copy |
| 77 | tee | ⛔ | ext:862 | |
| 78 | readlinkat | ✅ | syscalls.c:2658 | `/proc`/`/sys` readlink surface |
| 79 | newfstatat | ✅ | syscalls.c:2721 | `stat_m2l` + synthetic fakes |
| 80 | fstat | ✅ | syscalls.c:2785 | |
| 81 | sync | ✅ | ext:865 | |
| 82 | fsync | ✅ | ext:866 | |
| 83 | fdatasync | ✅ | ext:867 | → fsync |
| 84 | sync_file_range | 🟡 | ext:871 | → fsync, ignores range |
| 85 | timerfd_create | ✅ | ext:872 | `timerfd_shim.c` |
| 86 | timerfd_settime | ✅ | ext:873 | |
| 87 | timerfd_gettime | ✅ | ext:874 | |
| 88 | utimensat | ✅ | ext:875 | |
| 89 | acct | 🚫 | ext:883 | EPERM |
| 90 | capget | 🟡 | ext:884 | reports empty cap set |
| 91 | capset | 🚫 | ext:887 | EPERM |
| 92 | personality | 🟡 | ext:888 | accept, ignore |
| 93 | exit | ✅ | syscalls.c:2814 | |
| 94 | exit_group | ✅ | syscalls.c:2824 | |
| 95 | waitid | ✅ | ext:891 | |
| 96 | set_tid_address | ✅ | syscalls.c:2831 | |
| 97 | unshare | 🚫 | ext:912 | EPERM |
| 98 | futex | ✅ | syscalls.c:2834 | `do_futex` (full op set) |
| 99 | set_robust_list | 🟡 | syscalls.c:2838 | returns 0, no robust tracking |

### 100–199

| nr | name | status | handler | note |
|---|---|---|---|---|
| 100 | get_robust_list | ⛔ | ext:913 | |
| 101 | nanosleep | ✅ | ext:914 | |
| 102 | getitimer | ✅ | ext:916 | |
| 103 | setitimer | ✅ | ext:926 | |
| 104 | kexec_load | 🚫 | ext:942 | EPERM |
| 105 | init_module | 🚫 | ext:942 | EPERM |
| 106 | delete_module | 🚫 | ext:942 | EPERM |
| 107–111 | timer_create/gettime/getoverrun/settime/delete | ⛔ | ext:943 | POSIX timers ENOSYS |
| 112 | clock_settime | 🚫 | ext:947 | EPERM |
| 113 | clock_gettime | ✅ | syscalls.c:2841 | |
| 114 | clock_getres | ✅ | syscalls.c:2853 | |
| 115 | clock_nanosleep | ✅ | syscalls.c:2866 | |
| 116 | syslog | 🟡 | ext:948 | stub success |
| 117 | ptrace | 🚫 | ext:949 | EPERM — **major gap** |
| 118 | sched_setparam | 🟡 | ext:950 | no-op |
| 119 | sched_setscheduler | 🟡 | ext:950 | no-op |
| 120 | sched_getscheduler | 🟡 | ext:951 | returns 0 (SCHED_OTHER) |
| 121 | sched_getparam | 🟡 | ext:952 | returns 0 |
| 122 | sched_setaffinity | 🟡 | syscalls.c:2893 | accepts only unrestricted mask |
| 123 | sched_getaffinity | ✅ | syscalls.c:2912 | |
| 124 | sched_yield | ✅ | ext:955 | |
| 125 | sched_get_priority_max | ✅ | ext:956 | |
| 126 | sched_get_priority_min | ✅ | ext:956 | |
| 127 | sched_rr_get_interval | 🟡 | ext:967 | fixed 1 ms |
| 128 | restart_syscall | ⛔ | ext:970 | EINTR (never user-issued) |
| 129 | kill | ✅ | syscalls.c:2948 | pid-namespace guarded |
| 130 | tkill | ✅ | ext:973 | |
| 131 | tgkill | ✅ | ext:973 | |
| 132 | sigaltstack | 🟡 | ext:996 | shadow only |
| 133 | rt_sigsuspend | 🟡 | ext:997 | parks thread forever |
| 134 | rt_sigaction | 🟡 | syscalls.c:2964 | shadow only (recorded) |
| 135 | rt_sigprocmask | 🟡 | syscalls.c:2975 | shadow only |
| 136 | rt_sigpending | 🟡 | ext:1006 | always empty |
| 137 | rt_sigtimedwait | 🟡 | syscalls.c:2992 | always times out |
| 138 | rt_sigqueueinfo | 🟡 | ext:1010 | kills self |
| 139 | rt_sigreturn | 🟡 | syscalls.c:3002 | unreachable (no frames) |
| 140 | setpriority | ✅ | ext:1020 | |
| 141 | getpriority | ✅ | ext:1023 | |
| 142 | reboot | 🚫 | ext:1029 | EPERM |
| 143 | setregid | ✅ | ext:1032 | |
| 144 | setgid | ✅ | ext:1033 | |
| 145 | setreuid | ✅ | ext:1034 | |
| 146 | setuid | ✅ | ext:1035 | |
| 147 | setresuid | ✅ | ext:1036 | approximate (no saved-uid) |
| 148 | getresuid | ✅ | ext:1042 | |
| 149 | setresgid | ✅ | ext:1049 | |
| 150 | getresgid | ✅ | ext:1055 | |
| 151 | setfsuid | 🟡 | ext:1060 | returns geteuid |
| 152 | setfsgid | 🟡 | ext:1061 | returns getegid |
| 153 | times | ✅ | syscalls.c:3005 | |
| 154 | setpgid | ✅ | ext:1079 | |
| 155 | getpgid | ✅ | ext:1080 | |
| 156 | getsid | ✅ | ext:1083 | |
| 157 | setsid | ✅ | ext:1086 | |
| 158 | getgroups | ✅ | ext:1089 | |
| 159 | setgroups | ✅ | ext:1093 | |
| 160 | uname | ✅ | syscalls.c:3024 | fake "Linux 6.1.0 aarch64" |
| 161 | sethostname | 🚫 | ext:1094 | EPERM |
| 162 | setdomainname | 🚫 | ext:1094 | EPERM |
| 163 | getrlimit | ✅ | ext:1095 | subset (10..15 → unlimited) |
| 164 | setrlimit | ✅ | ext:1105 | subset |
| 165 | getrusage | ✅ | syscalls.c:3038 | |
| 166 | umask | ✅ | ext:1114 | |
| 167 | prctl | 🟡 | ext:1115 | subset of options |
| 168 | getcpu | 🟡 | ext:1116 | returns (0,0) |
| 169 | gettimeofday | ✅ | ext:1120 | |
| 170 | settimeofday | 🚫 | ext:1127 | EPERM |
| 171 | adjtimex | 🟡 | ext:1128 | reports TIME_OK |
| 172 | getpid | ✅ | syscalls.c:3049 | |
| 173 | getppid | ✅ | syscalls.c:3050 | |
| 174 | getuid | ✅ | syscalls.c:3059 | |
| 175 | geteuid | ✅ | syscalls.c:3060 | |
| 176 | getgid | ✅ | syscalls.c:3061 | |
| 177 | getegid | ✅ | syscalls.c:3062 | |
| 178 | gettid | ✅ | syscalls.c:3078 | guest tid |
| 179 | sysinfo | ✅ | syscalls.c:3079 | approximate |
| 180 | mq_open | ⛔ | ext:1134 | POSIX mq |
| 181 | mq_unlink | ⛔ | ext:1134 | |
| 182 | mq_timedsend | ⛔ | ext:1134 | |
| 183 | mq_timedreceive | ⛔ | ext:1134 | |
| 184 | mq_notify | ⛔ | ext:1134 | |
| 185 | mq_getsetattr | ⛔ | ext:1134 | |
| 186 | msgget | ⛔ | ext:1135 | SysV msg |
| 187 | msgctl | ⛔ | ext:1135 | |
| 188 | msgrcv | ⛔ | ext:1135 | |
| 189 | msgsnd | ⛔ | ext:1135 | |
| 190 | semget | ✅ | ext:1186 | forwarded |
| 191 | semctl | 🟡 | ext:1193 | cmd subset only |
| 192 | semtimedop | ⛔ | ext:1222 | |
| 193 | semop | ✅ | ext:1188 | forwarded |
| 194 | shmget | ✅ | ext:1147 | forwarded |
| 195 | shmctl | 🟡 | ext:1155 | IPC_RMID/IPC_STAT only |
| 196 | shmat | ✅ | ext:1149 | |
| 197 | shmdt | ✅ | ext:1153 | |
| 198 | socket | ✅ | net:1945 | |

### 199–293

| nr | name | status | handler | note |
|---|---|---|---|---|
| 199 | socketpair | ✅ | net:1947 | SEQPACKET→DGRAM fallback |
| 200 | bind | ✅ | net:1949 | |
| 201 | listen | ✅ | net:1951 | |
| 202 | accept | ✅ | net:1953 | |
| 203 | connect | ✅ | net:1957 | |
| 204 | getsockname | ✅ | net:1959 | |
| 205 | getpeername | ✅ | net:1961 | |
| 206 | sendto | ✅ | net:1963 | |
| 207 | recvfrom | ✅ | net:1966 | |
| 208 | setsockopt | ✅ | net:1968 | |
| 209 | getsockopt | ✅ | net:1971 | |
| 210 | shutdown | ✅ | net:1974 | |
| 211 | sendmsg | ✅ | net:1976 | |
| 212 | recvmsg | ✅ | net:1978 | |
| 213 | readahead | 🟡 | ext:1226 | ignore |
| 214 | brk | ✅ | syscalls.c:3102 | |
| 215 | munmap | ✅ | syscalls.c:3105 | |
| 216 | mremap | 🟡 | syscalls.c:3108 | no MREMAP_DONTUNMAP/FIXED |
| 217 | add_key | ⛔ | ext:1227 | |
| 218 | request_key | ⛔ | ext:1227 | |
| 219 | keyctl | ⛔ | ext:1227 | |
| 220 | clone | ✅ | shim.c:1027 | trap handler |
| 221 | execve | ✅ | ext:1229 | re-exec the shim |
| 222 | mmap | ✅ | syscalls.c:3148 | drm/snd mmap routed first |
| 223 | fadvise64 | 🟡 | ext:1241 | ignore |
| 224 | swapon | 🚫 | ext:1242 | EPERM |
| 225 | swapoff | 🚫 | ext:1242 | EPERM |
| 226 | mprotect | ✅ | syscalls.c:3166 | |
| 227 | msync | ✅ | ext:1243 | |
| 228 | mlock | ✅ | ext:1251 | |
| 229 | munlock | ✅ | ext:1252 | |
| 230 | mlockall | ✅ | ext:1253 | |
| 231 | munlockall | ✅ | ext:1254 | |
| 232 | mincore | ✅ | ext:1259 | pointer-validity oracle |
| 233 | madvise | 🟡 | syscalls.c:3169 | ignore |
| 234 | remap_file_pages | ⛔ | ext:1278 | dead on Linux too |
| 235 | mbind | 🟡 | ext:1279 | no-op |
| 236 | get_mempolicy | 🟡 | ext:1280 | MPOL_DEFAULT |
| 237 | set_mempolicy | 🟡 | ext:1283 | no-op |
| 238 | migrate_pages | ⛔ | ext:1284 | |
| 239 | move_pages | ⛔ | ext:1284 | |
| 240 | rt_tgsigqueueinfo | 🟡 | ext:1010 | kills self |
| 241 | perf_event_open | 🚫 | ext:1285 | EPERM |
| 242 | accept4 | ✅ | net:1955 | |
| 243 | recvmmsg | ✅ | net:1980 | |
| 260 | wait4 | ✅ | syscalls.c:3172 | |
| 261 | prlimit64 | 🟡 | syscalls.c:3187 | subset; NOFILE clamped |
| 262 | fanotify_init | 🚫 | ext:1304 | EPERM |
| 263 | fanotify_mark | 🚫 | ext:1304 | EPERM |
| 264 | name_to_handle_at | 🚫 | ext:1305 | EOPNOTSUPP |
| 265 | open_by_handle_at | 🚫 | ext:1305 | EOPNOTSUPP |
| 266 | clock_adjtime | 🚫 | ext:1306 | EPERM |
| 267 | syncfs | ✅ | ext:1307 | |
| 268 | setns | 🚫 | ext:1308 | EPERM |
| 269 | sendmmsg | ✅ | net:1983 | |
| 270 | process_vm_readv | ✅ | ext:1313 | memcpy (shared AS) |
| 271 | process_vm_writev | ✅ | ext:1313 | memcpy |
| 272 | kcmp | ⛔ | ext:1334 | |
| 273 | finit_module | 🚫 | ext:1335 | EPERM |
| 274 | sched_setattr | ⛔ | ext:1336 | |
| 275 | sched_getattr | ⛔ | ext:1336 | |
| 276 | renameat2 | ✅ | ext:1337 | EXCL/SWAP via renameatx_np |
| 277 | seccomp | ⛔ | ext:1354 | **major gap** |
| 278 | getrandom | ✅ | syscalls.c:3229 | arc4random_buf |
| 279 | memfd_create | ✅ | ext:1355 | mkstemp+unlink |
| 280 | bpf | 🚫 | ext:1356 | EPERM |
| 281 | execveat | ⛔ | ext:1357 | |
| 282 | userfaultfd | ⛔ | ext:1358 | |
| 283 | membarrier | 🟡 | ext:1359 | CMD_QUERY only |
| 284 | mlock2 | ✅ | ext:1364 | ONFAULT ignored |
| 285 | copy_file_range | ✅ | ext:1365 | bounce copy |
| 286 | preadv2 | 🟡 | ext:1374 | flags ignored |
| 287 | pwritev2 | 🟡 | ext:1378 | flags ignored |
| 288 | pkey_mprotect | ⛔ | ext:1382 | |
| 289 | pkey_alloc | ⛔ | ext:1382 | |
| 290 | pkey_free | ⛔ | ext:1382 | |
| 291 | statx | ✅ | ext:1383 | also aliased at **258** (see note) |
| 292 | io_pgetevents | ⛔ | ext:1385 | |
| 293 | rseq | ⛔ | syscalls.c:3237 | **gap** (glibc falls back) |

### 424–462 (post-5.x)

| nr | name | status | handler | note |
|---|---|---|---|---|
| 424 | pidfd_send_signal | ⛔ | ext:1432 | |
| 425 | io_uring_setup | ⛔ | ext:1433 | **major gap** |
| 426 | io_uring_enter | ⛔ | ext:1433 | |
| 427 | io_uring_register | ⛔ | ext:1433 | |
| 428 | open_tree | 🚫 | ext:1434 | EPERM |
| 429 | move_mount | 🚫 | ext:1434 | EPERM |
| 430 | fsopen | 🚫 | ext:1434 | EPERM |
| 431 | fsconfig | 🚫 | ext:1434 | EPERM |
| 432 | fsmount | 🚫 | ext:1434 | EPERM |
| 433 | fspick | 🚫 | ext:1434 | EPERM |
| 434 | pidfd_open | ⛔ | ext:1436 | |
| 435 | clone3 | ⛔ | ext:1437 | musl/glibc fall back to clone |
| 436 | close_range | ✅ | ext:1438 | skips shim-reserved fds |
| 437 | openat2 | ⛔ | ext:1478 | glibc falls back |
| 438 | pidfd_getfd | ⛔ | ext:1479 | |
| 439 | faccessat2 | ✅ | ext:1480 | |
| 440 | process_madvise | 🟡 | ext:1488 | returns 0 |
| 441 | epoll_pwait2 | ⛔ | ext:1489 | glibc falls back |
| 442 | mount_setattr | 🚫 | ext:1490 | EPERM |
| 443 | quotactl_fd | 🚫 | ext:1491 | EPERM |
| 444 | landlock_create_ruleset | ⛔ | (default ext:1499) | no handler |
| 445 | landlock_add_rule | ⛔ | (default ext:1499) | no handler |
| 446 | landlock_restrict_self | ⛔ | (default ext:1499) | no handler |
| 447 | memfd_secret | ⛔ | (default ext:1499) | no handler |
| 448 | process_mrelease | ⛔ | (default ext:1499) | no handler |
| 449 | futex_waitv | ⛔ | (default ext:1499) | no handler |
| 450 | set_mempolicy_home_node | ⛔ | (default ext:1499) | no handler |
| 451 | cachestat | ⛔ | (default ext:1499) | no handler |
| 452 | fchmodat2 | ✅ | ext:1492 | |
| 453 | map_shadow_stack | ⛔ | (default ext:1499) | no handler |
| 454 | futex_wake | ⛔ | (default ext:1499) | no handler |
| 455 | futex_wait | ⛔ | (default ext:1499) | no handler |
| 456 | futex_requeue | ⛔ | (default ext:1499) | no handler |
| 457 | statmount | ⛔ | (default ext:1499) | no handler |
| 458 | listmount | ⛔ | (default ext:1499) | no handler |
| 459 | lsm_get_self_attr | ⛔ | (default ext:1499) | no handler |
| 460 | lsm_set_self_attr | ⛔ | (default ext:1499) | no handler |
| 461 | lsm_list_modules | ⛔ | (default ext:1499) | no handler |
| 462 | mseal | ⛔ | (default ext:1499) | no handler |

> **`statx` @ 258 note:** `syscalls_ext.c:1383` maps both `258` and `291` to `do_statx`. `258` is inside the
> arm64 arch-reserved window (`244..259`) and is **not a real aarch64 syscall** — it is a deliberate alias
> (a guest observed issuing `statx` as `258`). Harmless, but worth removing once that guest is identified.

---

## 3. `/proc` synthesis inventory

Served in-process (only these paths):

| Path | Served by | Notes |
|---|---|---|
| `/proc/self/exe` | `syscalls.c:935` | real open; readlink at `:2664`; statx/newfstatat |
| `/proc/self/cmdline` | `syscalls.c:941` | exe path only (no real argv) |
| `/proc/self/maps` | `syscalls.c:948` / `proc_maps:829` | 4 synthesized lines (exe ro/rw, heap, stack) |
| `/proc/self/stat` | `syscalls.c:967` / `proc_stat:868` | 52 fields |
| `/proc/self/cwd` | `syscalls.c:2670` | readlink only |
| `/proc/self/fd/<N>`, `/proc/<pid>/fd/<N>` | `syscalls.c:710,2552` | fd-reopen (O_PATH chase) |
| `/proc/<pid>/root` | `syscalls.c:695` | rewritten to `/` |
| `/proc/cpuinfo` | `syscalls.c:954` / `proc_cpuinfo:885` | Apple M-series identity |
| `/proc/meminfo` | `syscalls.c:961` / `proc_meminfo:909` | MemTotal/Free/Available/Buffers/Cached/Swap |
| `/proc/asound/*` | `syscalls.c:974` | ALSA card enumeration |

**Missing (commonly probed — no handler → host rootfs → ENOENT):**

- Process identity: `/proc/self/status`, `/proc/self/environ`, `/proc/self/auxv`, `/proc/self/comm`,
  `/proc/self/smaps`, `/proc/self/statm`, `/proc/self/limits`, `/proc/self/io`, `/proc/self/mounts`,
  `/proc/self/mountinfo`, `/proc/self/cgroup`, `/proc/self/oom_score*`, `/proc/self/wchan`,
  `/proc/self/task/*`, `/proc/self/personality`, `/proc/self/sched`, `/proc/self/stack`.
- Other pids: `/proc/<pid>/{stat,status,maps,cmdline,fd,environ,task,root}` (only `fd` and `root` are special-cased).
- System-wide: `/proc/uptime`, `/proc/loadavg`, `/proc/stat`, `/proc/version`, `/proc/filesystems`,
  `/proc/devices`, `/proc/interrupts`, `/proc/partitions`, `/proc/diskstats`, `/proc/net/*`,
  `/proc/sys/*` (sysctl), `/proc/mounts`, `/proc/vmstat`, `/proc/zoneinfo`, `/proc/buddyinfo`,
  `/proc/pressure/*`, `/proc/keys`, `/proc/kmsg`, `/proc/modules`.
- **Directory enumeration:** `getdents64` on `/proc` reads the host rootfs dir, which is **empty**
  (`/Users/dperry/linux-shim/rootfs/proc` has no entries). `ls /proc`, `opendir("/proc")`, `lscpu`,
  `systemd`'s unit enumeration all see **nothing**.

## 4. `/sys` + `/dev` synthesis inventory

Served in-process (only these paths):

| Path | Served by | Notes |
|---|---|---|
| `/sys/dev/char/226:*/{uevent,device/{uevent,vendor,device,subsystem_vendor,subsystem_device},device/subsystem}` | `drmasahi.c` (kinds 2/4/5/6) | synthetic GPU PCI identity |
| `/sys/class/drm/{card0,renderD128}/{uevent,dev,subsystem}` | `drmasahi.c` (kinds 9/10/11) | libudev enumeration surface |
| `/sys/class/sound/card0/{uevent,dev,subsystem}`, `/run/udev/data/c116:0` | `snd_alsa.c` (kinds 1/2/3/6/7) | synthetic ALSA card |
| `/sys/class/input/event{0,1}/{uevent,dev,subsystem}`, `/run/udev/data/c13:{64,65}` | `evdev.c` (kinds 1/2/3/4/5) | synthetic input |
| `/sys/dev/char/13:{64,65}` symlinks | static (install.sh) | |
| `/dev/dri/{card0,renderD128}` | `drmasahi.c` | synthetic char 226:0/128 |
| `/dev/input/event{0,1}` | `evdev.c` | synthetic char 13:64/65 |
| `/dev/snd/{controlC0,pcmC0D0p,pcmC0D0c,pcmC0D1p,pcmC0D1c}` | `snd_alsa.c` | synthetic char 116:* |
| `/dev/video0` | `syscalls.c:2517` | synthetic (null-backed) |
| `/dev/tty`, `/dev/tty1..6`, `/dev/console`, `/dev/tty0` | `vt.c` | pty-backed VTs |
| `/dev/{ptmx,null,zero,full,random,urandom,tty,stdin,stdout,stderr,fd/*,pts/N}` | `passthru_dev:482` | forwarded to host |

**Missing (commonly probed):**

- `/sys/devices/system/cpu/*` (topology, online, possible, cpufreq), `/sys/devices/system/node/*`,
  `/sys/devices/virtual/*`, `/sys/bus/*`, `/sys/block/*`, `/sys/kernel/*`, `/sys/module/*`,
  `/sys/firmware/*`, `/sys/power/*`, `/sys/fs/cgroup` (cgroupfs — entire hierarchy absent),
  `/sys/fs/selinux`, `/sys/fs/bpf`.
- `/dev/shm` (glibc `shm_open`/POSIX shm target — **missing**; memfd works but `shm_open` fails ENOENT),
  `/dev/mqueue`, `/dev/loop*`, `/dev/kmsg`, `/dev/fuse`, `/dev/net/tun`.
- **Directory enumeration:** same as `/proc` — `getdents64` on `/sys` and subdirs reads the empty host
  rootfs. `libudev`'s `udev_enumerate_scan_devices` (used by libinput/PipeWire), `lspci`, `lsblk`,
  `systemd`, and `find /sys` all enumerate an **empty** tree. Only the exact leaf paths above resolve.

> `/dev` note: the rootfs `/dev` is empty; `/dev/snd`, `/dev/dri`, `/dev/input` placeholder entries are
> expected to be created by `install.sh`/boot scripts. The `getdents64` `d_type` fix at `syscalls.c:2214`
> only corrects `/dev/snd` entries that already exist as placeholders.

---

## 5. Tiered completeness checklist

### Tier 1 — must-have for the common aarch64 desktop / toolchain / runtime ecosystem

Mostly **done**. The gaps here are the ones that break *ordinary* binaries (debuggers, profilers,
shells, build tools).

| # | Item | Status / location | Why it matters |
|---|---|---|---|
| 1 | `ptrace` (117) → EPERM | `syscalls_ext.c:949` | gdb/lldb/strace/crash handlers cannot attach. **Highest-value single gap.** |
| 2 | `seccomp` (277) → ENOSYS | `syscalls_ext.c:1354` | Chromium/V8, systemd sandboxing, any sandboxed runtime probes and hard-fails. |
| 3 | `rseq` (293) → ENOSYS | `syscalls.c:3237` | glibc falls back, but every modern runtime leaves perf on the table; some (e.g. certain io_uring/DB) expect it. |
| 4 | `/proc/self/status`, `/proc/self/environ`, `/proc/self/auxv` | no handler | `systemd`, `qemu`, sanitizers, `pgrep`/`ps` read these; `environ`/`auxv` are used by `libunwind`, sanitizer runtimes, `getauxval`-based detection. |
| 5 | `/proc/self/mounts` + `mountinfo` | no handler | `df`, mount namespace checks (PipeWire flatpak probe), container runtimes. |
| 6 | `/proc/uptime`, `/proc/loadavg`, `/proc/stat` | no handler | `uptime`/`top`/`systemd` CPU accounting. |
| 7 | `/proc/sys/*` (sysctl) | no handler | `sysctl -a`, systemd tuning, many setup scripts. |
| 8 | `/proc/self/smaps` / `statm` | no handler | memory profiling (Valgrind/heaptrack/`ps -o rss`). |
| 9 | `get_robust_list` (100) | `syscalls_ext.c:913` ENOSYS | rarely called, but a pthread robust-mutex user will see it. |
| 10 | `execveat` (281) | `syscalls_ext.c:1357` ENOSYS | some loaders/fex use it; glibc usually falls back to `execve`. |
| 11 | `openat2` (437) | `syscalls_ext.c:1478` ENOSYS | glibc falls back; new apps using RESOLVE_* flags degrade (OK for now). |
| 12 | `statx` flags coverage | `syscalls_ext.c:1383` | implemented but ignores `AT_STATX_SYNC_AS_STAT`/`DONT_SYNC`; fine. |
| 13 | `prctl` option coverage | `syscalls_ext.c:1115` | missing `PR_SET_SECCOMP`, `PR_CAP_AMBIENT`, `PR_SET_PTRACER_ANY`, `PR_SET_NAME` per-thread is thread-local only. |

**Tier 1 verdict:** the *common desktop/runtime* path is effectively covered; the load-bearing missing
pieces are `ptrace`, `seccomp`, and the `/proc/self/*` identity files.

### Tier 2 — server / workload syscalls (io_uring, epoll, sendfile, vectored IO)

| # | Item | Status / location | Why it matters |
|---|---|---|---|
| 1 | `io_uring_setup/enter/register` (425–427) | `syscalls_ext.c:1433` ENOSYS | **The single largest server gap.** PostgreSQL/MySQL/MariaDB, QEMU, most async Rust/Go... apps that hard-require it fail; glibc does *not* fall back for code that calls it directly. |
| 2 | `io_pgetevents` (292) + aio `io_setup..io_getevents` (0–4) | `ext:1385`, `ext:704` ENOSYS | libaio (XFS/DB direct-IO paths). |
| 3 | `vmsplice` (75) / `tee` (77) | `ext:852`, `ext:862` ENOSYS | zero-copy pipelines; less critical (splice exists). |
| 4 | `sendfile` (71) / `splice` (76) / `copy_file_range` (285) | implemented (bounce copy) | functional, but not zero-copy; perf gap for file servers. |
| 5 | `epoll_pwait2` (441) | `ext:1489` ENOSYS | nanosecond epoll timeout; glibc falls back to epoll_pwait. |
| 6 | `preadv2`/`pwritev2` (286/287) flags | `ext:1374/1378` (flags ignored) | RWF_NOWAIT/DSYNC/HIPRI ignored — async direct-IO correctness. |
| 7 | `sched_setattr`/`sched_getattr` (274/275) | `ext:1336` ENOSYS | realtime/priority scheduling for latency-sensitive workloads. |
| 8 | `futex_waitv` (449), `futex_wake/wait/requeue` (454–456) | no handler | futex2 — new async runtimes use it. |
| 9 | `userfaultfd` (282) | `ext:1358` ENOSYS | live-migration / snapshotting / QEMU postcopy. |
| 10 | `bpf` (280) | `ext:1356` EPERM | eBPF tooling, systemd socket-activation filters. |
| 11 | `/proc/net/*` | no handler | `ss`/`netstat`/`/proc/net/tcp` for monitoring agents. |
| 12 | `/proc/diskstats`, `/proc/partitions`, `/proc/stat` | no handler | `iostat`/`vmstat`/Prometheus node_exporter. |

**Tier 2 verdict:** epoll/sendfile/vectored-IO/poll are done; the io_uring + aio + POSIX-timer cluster is
the concrete "run server workloads" gap.

### Tier 3 — esoteric: namespaces, cgroups, seccomp, ptrace, tracing, keys

| # | Item | Status / location | Why it matters |
|---|---|---|---|
| 1 | `clone3` (435) | `ext:1437` ENOSYS | musl/glibc fall back to clone; fine until a libc requires it. |
| 2 | `unshare` (97), `setns` (268) | `ext:912`, `ext:1308` EPERM | namespaces — containers, flatpak/snap. |
| 3 | `mount`/`umount2`/`pivot_root` (40/39/41), new mount API (428–433, 442) | EPERM | container runtimes, `mount --bind`. |
| 4 | `open_tree/move_mount/fsopen/fsconfig/fsmount/fspick/mount_setattr` | EPERM | newer mount API. |
| 5 | cgroupfs `/sys/fs/cgroup` + `statmount`/`listmount` (457/458) | no handler | systemd cgroup accounting, container resource limits. |
| 6 | `seccomp` (277) | ENOSYS | sandboxing (see Tier 1). |
| 7 | `ptrace` (117) | EPERM | debuggers (see Tier 1). |
| 8 | `kcmp` (272) | `ext:1334` ENOSYS | CRIU / checkpoint-restore. |
| 9 | `process_mrelease` (448), `process_madvise` (440→no-op) | ENOSYS / no-op | OOM-killer / live-migration of guest processes. |
| 10 | `add_key/request_key/keyctl` (217–219) | `ext:1227` ENOSYS | kernel keyring (NFS/ecryptfs/`keyctl`). |
| 11 | `landlock_*` (444–446) | no handler | unprivileged sandboxing (used by newer Chrome/systemd). |
| 12 | `mseal` (462) | no handler | V8/glibc hardening (newer kernels). |
| 13 | `memfd_secret` (447) | no handler | confidential-computing memfd. |
| 14 | `lsm_*` (459–461) | no handler | LSM self-attr (AppArmor/SELinux). |
| 15 | `pidfd_*` (424/434/438) | ENOSYS | needs a guest pid table (structural, see §1). |
| 16 | `cachestat` (451), `map_shadow_stack` (453) | no handler | niche. |
| 17 | SysV msg queues (186–189), POSIX mq (180–185), `semtimedop` (192) | ENOSYS | rarely needed; forward-to-host like shm/sem is feasible (see below). |
| 18 | `perf_event_open` (241) | `ext:1285` EPERM | `perf`/`perf stat` profiling. |
| 19 | `fanotify_*` (262/263), `name_to_handle_at`/`open_by_handle_at` (264/265) | EPERM/EOPNOTSUPP | file access monitoring / NFS handles. |
| 20 | `/proc/sys/kernel/*`, `/proc/keys`, `/proc/modules`, `/proc/kmsg` | no handler | kernel introspection. |

**Tier 3 verdict:** the whole namespaces/cgroups/seccomp/ptrace/keyring/tracing surface is intentionally
refused or ENOSYS. For "run every aarch64 binary" this is the frontier: containers (Docker-style) and
heavy sandboxing are currently impossible; the rest of Tier 3 is long-tail.

---

## 6. Recommendations for the "complete the ABI" phase (seeding notes — no code changed)

1. **Signals first, then everything built on them.** Without signal-frame delivery, `ptrace`, `seccomp`,
   `userfaultfd`, and `PERF`-style faults cannot be real. Decide whether to (a) deliver signals via a
   synthesized `rt_sigreturn` frame + `sigreturn` trampoline, or (b) emulate the *observable effects* of
   the signal syscalls and keep the no-frame model. This decision gates Tier 1 items 1–2 and Tier 3 items 6–7.
2. **A guest pid table** is the prerequisite for `pidfd_*`, correct `/proc/<pid>/*`, and `process_mrelease`.
   It already exists implicitly (`guest_tid_known`, `guest_child_known`) — promote it to a real table.
3. **io_uring** should be mapped onto the existing kqueue/bounce machinery first (io_uring→kqueue+workers),
   as that unblocks the largest Tier-2 surface; `io_pgetevents`/aio follow the same backend.
4. **`/proc` + `/sys` directory enumeration** is the highest-leverage `/proc`/`/sys` fix: synthesize
   `getdents64` for `/proc`, `/proc/<pid>`, `/sys/class/*`, `/sys/dev/char`, `/sys/devices/system/cpu`,
   `/dev`, `/dev/snd`, `/dev/dri`, `/dev/input`. Leaf files already exist; the tree structure does not.
5. **`/dev/shm`** is a one-line passthrough/synthesized dir that unblocks POSIX `shm_open`.
6. **Forward-to-host candidates** (same technique already used for `semget`/`shmget`/`shmat`):
   SysV `msgget/msgsnd/msgrcv/msgctl` and POSIX `mq_*` are available on macOS and are structurally safe
   (guest pointers are host pointers) — cheaper than the current ENOSYS.

---

*Audit produced by reading `src/syscalls.c`, `src/syscalls_ext.c`, `src/net.c`, `src/shim.c`,
`src/drmasahi.c`, `src/snd_alsa.c`, `src/evdev.c`, and the Linux v6.10 asm-generic unistd.h table.
No shim source was modified.*
