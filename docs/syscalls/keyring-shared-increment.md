# Shared guest key object namespace

`src/guest_key_shared.c` adds a bounded, file-backed key **object** namespace
under an explicitly supplied private guest directory. Each operation opens a
fresh file description and locks it, so a host fork does not accidentally
share a `flock` owner. The directory must be absolute, owned by the current
host account, and inaccessible to group/other; the fixed database file must
be a regular, single-link, owner-only file opened without following symlinks.
It stores a monotonic serial, owner guest UID, bounded description/payload,
and revocation state. It never touches a host keyring or Keychain.

The synthetic ARM64 test forks, re-execs itself, finds and reads the same
serial in the child, rejects a different guest UID, revokes there, then
confirms the parent sees revocation. A second private directory sees none of
the first namespace's keys. The test does not use any real credentials.

This remains a dependency, **not syscall support**. `add_key`, `request_key`,
and `keyctl` still return `ENOSYS`. Keyring membership, Linux permission
masks, key expiration, quotas, special keyring IDs, keyctl command coverage,
and upcall behavior are still absent. In particular, a shared object ID alone
does not prove that a guest process is subscribed to its keyring. Do not wire
the dispatcher to this store or count 217–219 as compatible until a guest
ARM64 cross-call test proves add → request/search → keyctl read/revoke across
processes, with guest permissions and lifetime behavior. A private Arch boot
test follows that integration.

The file is owner-only but not encrypted at rest. A complete design must
decide whether payload persistence is acceptable and use stronger protection
if not. Writes are in place: a crash can leave a valid-looking but incomplete
record, and this prototype has neither checksums nor transactional recovery.
Do not use it for real key material or expose it through the syscall ABI yet.

Focused native ARM64 test:

```sh
clang -arch arm64 -O2 -Wall -Wextra -Werror -Isrc \
  tests/guest-key-shared.c src/guest_key_shared.c -o /tmp/shimmy-guest-key-shared-test
/tmp/shimmy-guest-key-shared-test
```
