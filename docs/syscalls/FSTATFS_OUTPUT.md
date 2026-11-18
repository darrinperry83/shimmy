# ARM64 `fstatfs` (#44) output

The existing handler wrote the Linux 120-byte result directly through a guest
pointer. That included its synthetic pidfd filesystem result. The handler now
stages either result locally, then uses checked guest copyout. File-descriptor
errors retain their ordering before output validation.

The disposable ARM64 probe checks an ordinary guest-root file descriptor, a
synthetic pidfd, null and unmapped destinations, an invalid descriptor, and a
canary after the result. It does not establish full Linux filesystem statistic
parity; the host path remains translated through `statfs_m2l`, and pidfds
continue to report the existing anon-inode filesystem identity.

The isolated branch passed a CLI build and private Arch boot with a fresh
login prompt and settled D-Bus broker/launcher. The harness stopped its exact
guest and the disposable rootfs clone was removed. Protected fixture entries
caused copy warnings, so this is a boot smoke test rather than proof of full
rootfs fidelity.

```sh
./build.sh cli
python3 tests/run-fstatfs-output.py --sysroot /path/to/arch-rootfs
```
