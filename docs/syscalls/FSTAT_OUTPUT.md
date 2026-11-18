# ARM64 `fstat` (#80) output

The existing handler wrote the Linux 128-byte stat directly through a guest
pointer. It also adjusted that same pointer for synthetic DRM, sound, input,
and virtual-console descriptors. It now stages the translated stat locally,
applies the existing synthetic overlays there, and copies the complete result
through checked guest memory. The synthetic identity choices are unchanged.

The disposable ARM64 probe checks a regular guest-root file, null and
unmapped outputs, invalid-fd error, and a canary after the 128-byte result.
It does not establish parity for every synthetic descriptor or ownership and
inode translation. The adjacent path-based `newfstatat` (#79) remains separate.

The isolated branch passed a CLI build and private Arch boot with a fresh
login prompt and settled D-Bus broker/launcher. The harness stopped its exact
guest and the disposable rootfs clone was removed. Protected fixture entries
caused copy warnings, so this is a boot smoke test rather than full rootfs
fidelity.

```sh
./build.sh cli
python3 tests/run-fstat-output.py --sysroot /path/to/arch-rootfs
```
