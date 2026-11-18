# ARM64 `statfs` (#43) output memory

The main dispatcher already resolved a guest path and translated Darwin's
`statfs` result to the 120-byte Linux ARM64 structure. It wrote that structure
directly through the guest pointer. The handler now stages the translated
structure and writes it through the checked guest-memory copyout path.

The disposable ARM64 probe checks a valid `/tmp` query, null and unmapped
output addresses (`EFAULT`), a missing pathname (`ENOENT`), the translated
filesystem type and block size, and a canary immediately beyond the 120-byte
output. The isolated CLI build and probe pass. A private Arch boot reached a
fresh login prompt with the D-Bus broker and launcher settled. The harness
stopped its scoped guest and the exact clone was removed. The fixture copy
emitted protected-path permission warnings, so the boot is a smoke test.

The existing path resolver, filesystem identity approximation, and `fstatfs`
handler (#44) are unchanged.

```sh
./build.sh cli
python3 tests/run-statfs-output.py
```
