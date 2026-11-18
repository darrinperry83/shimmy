# Private keyring membership dependency

The shared object store now supports explicit, owner-only keyring objects:
create a ring, link a key, search linked `user` keys, and unlink. An unlinked
key is invisible to ring search. Owner checks apply to both the ring and key;
revoked keys are excluded from search. The ring and key serials share one
monotonic namespace and persist across host fork and shim-style re-exec.

`tests/guest-key-shared.c` uses only synthetic data. Its child re-execs,
searches the parent's ring, reads the linked key, and revokes it; the parent
then observes the revocation. It also checks unlinked keys, duplicate links,
wrong-owner denial, namespace isolation, and insecure file/directory modes.

This is **not** a Linux keyring implementation. No guest process is yet
subscribed to thread/process/session rings; there are no Linux key permission
masks, expiry, quota policy, possession checks, or upcall. Dispatch for
`add_key`, `request_key`, and `keyctl` remains `ENOSYS`. Enabling the calls now
would still give incomplete lifetime and authorization semantics.

The on-disk prototype format is version 2 and rejects version 1 rather than
silently interpreting old data. It still performs in-place writes, has no
transaction journal or authenticated checksum, and stores payload bytes
without encryption. The namespace is private to the host account, but these
limitations must be resolved before storing real secrets or exposing the
guest ABI. Tests must continue to use disposable synthetic payloads.
