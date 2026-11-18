# ARM64 `symlinkat` (#36)

The extended dispatcher used to pass the Linux guest's target string directly
to Darwin `symlinkat`. A host mapped address is not necessarily guest owned,
and a bad guest address could crash the shim or expose a host string. The
handler now snapshots both guest strings with the existing bounded guest
memory helper before resolving the new link's location. The target text is
stored verbatim; it is not translated into a host path.

The disposable ARM64 probe checks invalid guest target and link pointers,
an unterminated target at `PATH_MAX`, an existing link, and the exact link
text through both `readlinkat` and the host test fixture. It passed with a
CLI build. An isolated private Arch boot reached a fresh login prompt with
the D-Bus broker and launcher settled. The harness stopped its scoped guest
and the exact rootfs clone was removed. The fixture clone emitted protected
path permission warnings, so the boot is a smoke test, not a rootfs fidelity
proof.

This change does not claim complete pathname, dirfd, or filesystem parity.
The existing destination resolver and host `symlinkat` behavior remain in
place. It creates and checks links only inside a disposable test root.

```sh
./build.sh cli
python3 tests/run-symlinkat.py
```
