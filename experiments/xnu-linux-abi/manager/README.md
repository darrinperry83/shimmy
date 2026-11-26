# Minimal systemd manager reference

`reference.py` runs the pinned, unchanged Arch systemd package as actual PID 1
inside a disposable Linux container. It uses a custom target and one service,
with generators disabled and a private runtime directory. Docker is a Linux
reference tool only; the intended OS still runs directly on XNU.

The positive control executes original Arch printf, captures its marker, requires
systemd's SIGCHLD and matching child-exit log, and checks manager exit zero.
The negative control executes original Arch false, requires child exit one and
manager exit 23 through FailureActionExitStatus. Both controls pass with systemd
262.2-1 from immutable image
`sha256:0151e70bd01e7e4c891e903edf1dacba81656f04c6150bfce78d5a570e4df443`.
Generated units, raw/normalized logs, package hashes and `reference.json` live in
ignored `out/systemd-manager/`.

```sh
python3 experiments/xnu-linux-abi/manager/reference.py
```

This establishes a reproducible Linux service-lifecycle target. It is not an
XNU manager pass. The [native process checkpoint](../process/README.md) ports
fork, exec and wait semantics that underpin this target. Manager event/signal
interfaces, rootfs/proc/cgroup views and coherent init lifecycle remain work.
The XNU process profile also makes a separate `systemd --user` startup attempt
with console logging; its log and nonzero result are diagnostic evidence,
not a fake PID 1, boot marker or service success.

The current XNU attempt on kernel 65B0EF6B-910A-3305-A3A2-2AD23BDAC70A exits
one at the same unbooted-context guard as the Linux control. This happens in
[systemd's safety checks](https://github.com/systemd/systemd/blob/v262.2/src/core/main.c).
See [evidence.json](evidence.json). The next manager boundary is a coherent Linux
init/runtime context, together with the required event and filesystem views;
changing only getpid or creating a boot marker would not establish that context.

The [direct-exec prerequisite](../interp/README.md) now passes on XNU:
normal Arch executable paths use kernel PT_INTERP with native chroot lookup,
without a userspace loader command. This also runs unchanged systemd --version
inside the minimal Arch root. Manager/init identity, proc/cgroup and event
interfaces remain separate work; no boot marker or PID 1 override was added.

The first native event-loop primitive is now qualified separately in
[events/README.md](../events/README.md): eventfd2 plus bounded ppoll, with
real kqueue readiness and native descriptor lifetime. This is not an epoll
implementation and does not change the manager's unbooted-context result.

The subsequent [native batch](../reactor/README.md) now passes bounded epoll,
timerfd, signalfd and private futex controls on XNU, followed by a callback
application using unchanged Arch libsystemd. Its one-shot IO, signal and timer
callbacks complete with zero exit and empty stderr. See
[batch evidence](../reactor/evidence.json). This qualifies the library event
loop; the separate manager diagnostic still exits one at the unbooted-context
guard. The next boundary is the coherent Linux init/runtime context and required
proc/cgroup/rootfs views, with complete thread/signal/futex semantics still work.
