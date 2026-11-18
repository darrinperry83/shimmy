# ARM64 syscall 461: `lsm_list_modules`

Shimmy currently models no Linux Security Modules. The
[Linux syscall](https://github.com/torvalds/linux/blob/v6.12/security/lsm_syscalls.c#L78-L113)
explicitly allows zero active modules: it writes zero to the caller's `u32`
byte-size pointer and returns zero elements, leaving the IDs buffer untouched.

This handler implements that zero-module query with checked guest pointer
access and `EINVAL` for nonzero flags. It does not install an LSM or implement
`lsm_get_self_attr` / `lsm_set_self_attr`; callers needing a security context
cannot infer one from this result. Revisit if Shimmy models any LSM.

Verification: `./build.sh cli` and `python3 tests/run-lsm-list-modules.py`.
The raw ARM64 probe covers a nonempty buffer, zero-size query, invalid flags,
null/bad size pointers, and an untouched ID sentinel.
