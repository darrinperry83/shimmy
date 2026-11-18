# ARM64 syscall 217: `add_key` needs a guest keyring service

`src/syscalls_ext.c` currently returns `-ENOSYS` for all three Linux key
retention calls: `add_key` (217), `request_key` (218), and `keyctl` (219).
`src/test_syscalls_ext.c` asserts that refusal for 217. This is an honest
compatibility gap. `add_key` alone must not return a plausible serial number:
without a shared `keyctl` implementation, a guest cannot read, search, link,
update, revoke, or inspect that key. Returning a host keychain identifier
would expose a different authority and lifetime model.

Linux keys are more than payload blobs. The kernel assigns positive serials,
types, descriptions, owner UID/GID, permissions, expiry and state. Keyrings
contain links to keys; thread, process, session and user keyrings have distinct
inheritance and lookup behavior. `request_key` searches the subscribed rings
and may invoke a userspace construction helper. The existing Shimmy guest
credential virtualization and fork/re-exec path therefore have to participate
in one common implementation. The installed Arch `linux/keyctl.h` lists the
special keyring IDs and the `KEYCTL_*` operations that callers may request.

## Minimum private implementation contract

* Give each Shimmy guest group its own key namespace and allocator. Never
  forward guest serials, payloads, or special keyring IDs to the host keychain
  or a host-global key service. An unrelated Mac process and a second guest
  group must not be able to inspect or guess each other's keys.
* Start with an explicitly bounded `user` key type, a private session ring,
  and the keyctl operations needed to obtain, search, read, update, revoke and
  unlink a key. Return Linux errors for unsupported types or operations; do
  not advertise general keyring parity. Preserve the `logon` type's unreadable
  payload rule if it is ever added.
* Check each operation against guest UID/GID, supplementary groups, possession
  and the key's permission mask. Limit payload bytes, key count, description
  length and nesting depth. Handle expiry/revoke/negative keys without
  returning stale contents. Zeroize released payloads and keep the store out
  of logs and unprotected environment variables.
* Define ownership across guest threads, fork, re-exec, session changes and
  abnormal shim exit before exposing serials. In this project `guest_execve`
  re-execs the shim as a fresh host process; ordinary in-process static tables
  would be lost. A scoped broker or other shared state mechanism is required
  for the Linux lifetime contract.
* Keep `request_key` lookup and construction semantics explicit. A lookup-only
  first phase could avoid launching a request-key helper, but an absent key
  must report an appropriate miss; it must not silently synthesize a key or
  claim helper support.

## Required proof before enabling 217

Use two independent guest groups and distinct guest users. Prove that a key
added in one process can be found and read through the permitted companion
calls after fork/re-exec, while a guessed serial and a second group cannot
access it. Test duplicate type/description replacement, wrong permission,
expired and revoked keys, payload size bounds, unsupported type, invalid
guest pointers, process/session ring lifecycle, and broker crash cleanup.
Run an actual Arch consumer plus the isolated boot gate after unit tests.
Do not print test payloads or use the user's live secrets.

This branch documents the shared blocker and leaves the runtime `-ENOSYS`
unchanged. It does not claim that Linux key retention works yet.

Primary semantics: [Linux kernel key retention documentation](https://www.kernel.org/doc/html/latest/security/keys/core.html),
[Linux `add_key(2)` manual](https://man7.org/linux/man-pages/man2/add_key.2.html),
and the installed Arch `usr/include/linux/keyctl.h`.
