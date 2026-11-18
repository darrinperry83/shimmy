# ARM64 `ftruncate` (#46) negative length

The existing handler enforces virtual memfd shrink/grow seals before calling
Darwin `ftruncate`. A negative signed length must return Linux `EINVAL`; on
a sealed file, comparing that value to the current size could instead return
`EPERM`. The handler now validates the signed length before seal or fd work.

The disposable ARM64 probe checks negative length on a valid and invalid fd,
actual shortening and zero extension on a file inside a temporary guest root,
and the important sealed-memfd ordering: negative length is `EINVAL`, while
a valid shrink remains `EPERM`. The CLI build and probe pass. A private Arch
boot reached a fresh login prompt with the D-Bus broker and launcher settled.
The harness stopped its scoped guest and the exact clone was removed. The
fixture copy emitted protected-path permission warnings, so this is a boot
smoke test rather than a rootfs fidelity proof.

The existing virtual seal implementation and guest fd routing are unchanged.
This is a narrow ABI correction, not a claim of complete memfd or file-seal
compatibility.

```sh
./build.sh cli
python3 tests/run-ftruncate-length.py
```
