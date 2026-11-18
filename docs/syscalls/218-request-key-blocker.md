# ARM64 syscall 218: `request_key` family blocker

Status: **unsupported**. `src/syscalls_ext.c` currently returns `-LX_ENOSYS`
for `add_key` (217), `request_key` (218), and `keyctl` (219). This note does
not change runtime behavior or count #218 as compatible.

`request_key(type, description, callout_info, dest_keyring)` searches keyrings
subscribed to the caller for an accessible, matching key. If found, it returns
that key's serial and may link it to the destination keyring. If not found,
Linux can use `callout_info` to construct a key through an authorization key
and a user-space request-key upcall. With no callout information, the missing
key is an error. This is a search, permission, lifetime, and potentially
process-creation interface, not simply an ID allocator.

Shimmy currently has no guest-private key registry or keyring graph. Its
virtual `RDUID0` credentials track real/effective/saved UID and GID, while
host file access still uses the Mac user's identity (`src/syscalls.h`).
Returning a synthetic serial would leave `keyctl` unable to read, describe,
link, or revoke it. Returning `ENOKEY` unconditionally would also lie when a
guest key should be found. Forwarding to the Darwin host or sharing key
material through a global file would cross the guest/host boundary and could
expose secrets. The existing `ENOSYS` is the honest result for now.

Implement this as one isolated key-management subsystem across 217–219:

1. Give each guest instance a private namespace of virtual key serials, key
   objects, and thread/process/session keyrings. Define fork, exec, and exit
   inheritance and cleanup, including the shim's re-exec path.
2. Apply guest UID/GID and key permission checks on every search, link, read,
   update, and revoke operation. Include expiry, revocation, quotas, and
   bounded descriptions/payloads before allowing untrusted guest input.
3. Support a documented subset of `add_key` and `keyctl` sufficient to make
   a key discoverable and usable. Only then implement `request_key` search
   with `callout_info == NULL`, including special destination keyring IDs,
   correct missing-key and access errors, and no host keyring fallback.
4. Treat non-null `callout_info` as a separate upcall milestone: the guest
   helper, authorization key, timeout, error propagation, and concurrent
   construction must work before advertising it. Do not run a host helper
   with guest-supplied key material as a shortcut.

The family regression should add a disposable synthetic key in guest A,
retrieve it by `request_key`, verify `keyctl` can inspect it, then check a
missing key, wrong type, permission denial, expiry/revocation, fork/exec
lifecycle, two independent guest instances, and host isolation. Test invalid
guest pointers and maximum input sizes. Do not use real credentials or dump
payloads to logs. After isolated tests, run the private Arch boot/login
regression. Until the family passes those gates, retain `ENOSYS` for #218.

Primary references: [Linux kernel key management](https://docs.kernel.org/security/keys/core.html)
and [request-key mechanism](https://docs.kernel.org/security/keys/request-key.html).
