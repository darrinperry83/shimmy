# Guest-private key object store increment

`src/guest_key_store.c` is a dependency for Linux key syscalls 217–219, not a
syscall implementation. Its explicit `gk_store` handle owns bounded `user`
key objects with monotonic serials, owner-only lookup/read/revoke, and payload
erasure on revoke and destruction. Separate handles cannot see each other's
objects. The isolated `tests/guest-key-store.c` checks those properties and
the key count/payload limits without reading any real key material.

No dispatcher or build target uses this primitive yet. The existing `ENOSYS`
responses remain. In particular, the primitive has **no keyring links**,
permission masks, expiry, or cross-process state. Shimmy uses a host `fork()`
and re-execs itself for guest `execve`, so wiring this in-process object store
to `add_key` now would return serials that other guest processes cannot use.
It cannot be counted as Linux `add_key`, `request_key`, or `keyctl` support.

The next family increment needs a per-guest session namespace shared by shim
processes, thread/process/session keyring membership, credential-aware
permission checks, fork/exec/exit lifetime rules, and a keyctl read/search
path. Then an `add_key` ID must be searchable by `request_key` and usable by
`keyctl` in another guest process. A request-key upcall is a later, separate
milestone. Keep host keyrings, Keychain, and real credentials out of tests.

Focused test command on macOS:

```sh
clang -arch arm64 -O2 -Wall -Wextra -Werror -Isrc \
  tests/guest-key-store.c src/guest_key_store.c -o /tmp/shimmy-guest-key-store-test
/tmp/shimmy-guest-key-store-test
```
