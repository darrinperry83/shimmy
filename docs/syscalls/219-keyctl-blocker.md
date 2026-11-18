# ARM64 syscall 219: `keyctl` needs a private keyring family

`src/syscalls_ext.c` routes `add_key` (217), `request_key` (218), and
`keyctl` (219) together to `-LX_ENOSYS`. No guest key registry or keyring
broker exists. `keyctl` is a multiplexed interface, not a single operation:
the installed Linux `keyctl.h` defines ring selection and joining, search,
link/unlink, read and describe, update, revoke and invalidate, permissions,
timeout, and capability queries. Returning success for an isolated command or
inventing a serial number would suggest that companion operations work when
they do not.

Do not forward these calls to the host keychain or a host Linux keyring. A
guest's serial numbers, payloads, permissions, and revocation must be private
to its Shimmy instance. Shimmy can report virtual guest credentials under
`RDUID0` (`src/syscalls.h`); host UID alone cannot authorize access to guest
keys. A later implementation needs a shared registry for all three syscalls,
with virtual serials; bounded `user` payloads; thread, process, session, and
user ring ownership; checked guest copy-in/copy-out; permission checks using
guest credentials; and quota, expiry, revocation, fork/exec, and cleanup
rules. Special `KEY_SPEC_*` IDs must resolve within that registry. Keep the
request-key upcall and authorization commands unsupported until an isolated
guest upcall path exists.

The first coherent milestone should implement `add_key` plus `keyctl` ring
selection, link, search, read, describe, update, and revoke against this
private backend. Only then can `request_key` with no callout return a useful
existing key or a truthful miss. Tests must compare these operations with a
Linux ARM64 oracle; cover invalid pointers, IDs, lengths, permissions and
special selectors; cross-process fork/exec sharing and isolation; quota,
expiry and revocation; and two independent Shimmy groups alongside an
unrelated host process. Verify that guest payloads never appear in host
keyrings or another Shimmy group, then run the private Arch boot gate.

**Status:** no runtime change in this branch. Syscall 219 remains `ENOSYS`
and must not be counted as compatible. No host keyring was accessed.
