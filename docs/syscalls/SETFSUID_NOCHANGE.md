# ARM64 `setfsuid` (#151): no-change subset

Shimmy has no separate guest filesystem UID and does not enforce guest DAC on
Darwin-backed files. Linux returns the previous fsuid even if a requested
change fails, so the prior handler's unconditional euid return could make a
root guest believe a different fsuid was installed. This branch only returns
the current effective UID for a `-1` query or a request for that same ID. A
different requested UID returns `EOPNOTSUPP` without changing host or guest
credentials. It does not pretend that a UID switch occurred.

The disposable ARM64 probe checks query, same-ID no-op, explicit refusal and
unchanged effective identity in ordinary and `RDUID0=1` virtual-root modes.
This is not full Linux `setfsuid`: a separate fsuid state, guest file access
checks, creation ownership, fork/exec inheritance and permission rules are
still required. See `151-setfsuid-blocker.md` for the existing architecture
review. The isolated branch passed a private Arch boot with a fresh login
prompt and settled D-Bus broker/launcher. The harness stopped its exact guest
and removed the disposable rootfs clone. Protected fixture entries caused
copy warnings, so this is a boot smoke test rather than full rootfs fidelity.

```sh
./build.sh cli
python3 tests/run-setfsuid-nochange.py --sysroot /path/to/arch-rootfs
```
