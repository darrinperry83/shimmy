# ARM64 `unshare` (#97): zero flags only

Linux defines `unshare(0)` as a no-op that succeeds. Shimmy previously returned
`EPERM` for every invocation. This branch returns zero only for flags exactly
zero. Every nonzero request still receives `EPERM`; no filesystem, process,
user, network, or mount namespace is created or changed.

The disposable ARM64 guest probe checks that `unshare(0)` succeeds while its
PID, UID, working directory, and an open pipe stay usable. It also checks that
`CLONE_NEWNS` is refused. This is not general namespace compatibility; Linux
may distinguish invalid flags from permission errors in cases still refused
by Shimmy.
An isolated private Arch clone reached a fresh login prompt with D-Bus broker
and launcher settled. The fixture copy warned about protected `/run` entries,
so this is a boot smoke test rather than full rootfs fidelity proof.

```sh
./build.sh cli
python3 tests/run-unshare-zero.py --sysroot /path/to/arch-rootfs
```
