# ARM64 syscall 460: `lsm_set_self_attr`

Shimmy has no active Linux Security Module hooks. The
[Linux path](https://github.com/torvalds/linux/blob/v6.12/security/security.c#L3915-L3965)
rejects reserved flags, context sizes outside `sizeof(lsm_ctx)` to
`PAGE_SIZE`, unreadable guest data and inconsistent `len`/`ctx_len`, then
returns `EOPNOTSUPP` when no module handles the request.

This branch follows that validation path using checked guest input and the
16 KiB guest page size. It never reports a security-label change. If Shimmy
later models an LSM, this handler must be replaced together with the module
list and attribute query.

Verification: `./build.sh cli` and `python3 tests/run-lsm-set-self-attr.py`.
The raw ARM64 test covers valid refusal and malformed flag, size, pointer and
length cases. No live boot result is claimed.
