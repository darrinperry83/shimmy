# `pkey_alloc` (ARM64 syscall 289): unavailable hardware keys

Shimmy's ARM64 guest currently returns `ENOSYS` for `pkey_alloc` and
`pkey_free`. This is the truthful behavior for a guest without architecture
protection keys. The existing `pkey_mprotect` subset supports only the default
key `0` and legacy key `-1`, routing them through ordinary guest `mprotect`.

Returning a made-up positive key would be unsafe: applications use a key to
apply separate memory permissions and can later change access without a new
`mprotect` call. Neither Shimmy's guest memory model nor the Apple Silicon host
provides that Linux per-thread key access contract. Merely recording the key in
a table would make `pkey_mprotect` appear to install protection it cannot
enforce. The companion `pkey_free` must share the same per-address-space
allocation, VMA, fork, and lifecycle state.

No runtime change is made in this branch. Do not count syscall 289 as
implemented. Revisit only if a complete enforceable protection-key backend is
designed and tested with multiple guest threads and permission transitions.

Primary source: Linux `mm/mprotect.c` defines `pkey_alloc` and `pkey_free`
under `CONFIG_ARCH_HAS_PKEYS` and uses `arch_set_user_pkey_access` after
allocating a key. See
https://github.com/torvalds/linux/blob/master/mm/mprotect.c#L955-L1006.
