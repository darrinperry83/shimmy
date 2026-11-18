# ARM64 `capget` (#90) guest memory

The existing capability query negotiated the Linux header version but read
and wrote guest pointers directly. This branch stages the eight-byte header,
writes an unknown-version negotiation result through checked guest copyout,
and writes the established zero-capability v1 or v2/v3 data through a bounded
checked copy. It does not change the existing capability policy.

The disposable ARM64 probe covers unknown-version negotiation, v3 and v1 data
sizes, bad header and data addresses, and canaries around the output. It does
not establish cross-process capability queries or `capset` parity; those need
a coherent guest credential and privilege model.

The isolated branch passed a CLI build and private Arch boot: a fresh login
prompt appeared and the D-Bus broker and launcher settled. The harness stopped
its exact guest and the disposable rootfs clone was removed. Protected
fixture entries produced copy warnings, so this is a boot smoke test rather
than proof of full rootfs fidelity.

```sh
./build.sh cli
python3 tests/run-capget-memory.py --sysroot /path/to/arch-rootfs
```
