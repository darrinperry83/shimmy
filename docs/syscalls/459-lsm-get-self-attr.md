# ARM64 syscall 459: `lsm_get_self_attr`

Shimmy has no active Linux Security Module hooks. In
[Linux's implementation](https://github.com/torvalds/linux/blob/v6.12/security/security.c#L3824-L3913),
the zero-hook path validates the attribute, size pointer and optional
`LSM_FLAG_SINGLE` context ID, writes zero to `*size`, then returns
`EOPNOTSUPP` from the default hook. This branch matches that path with
checked guest memory access. It never claims to supply a security label.

`lsm_list_modules` separately reports zero active modules. If Shimmy later
models an LSM or self attributes, this handler must be extended alongside
that list and the security policy itself.

Verification: `./build.sh cli` and `python3 tests/run-lsm-get-self-attr.py`.
The raw ARM64 test covers zero-hook response, size writeback, flags, ID,
undefined attribute, and bad guest pointers. No live boot claim is made.
