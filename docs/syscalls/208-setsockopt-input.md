#208 `setsockopt`: bounded guest input

The native AArch64 `setsockopt(fd, level, optname, optval, optlen)` handler previously passed the guest `optval` pointer directly to the option decoder or Darwin `setsockopt`. An invalid guest address could therefore be dereferenced by host code. The handler now copies a nonempty payload through the checked guest-memory API before option translation or socket mutation. It rejects a missing nonempty pointer with `EFAULT` and refuses payloads above 64 KiB with `EOPNOTSUPP`. Zero-length null versus nonnull input remains distinct.

`tests/guest-setsockopt-input.c` is a disposable no-libc AArch64 probe. It creates a UDP socket, reads `SO_RCVBUF`, verifies an invalid pointer and null nonempty value return `EFAULT`, checks that the invalid pointer did not alter the option, verifies the size bound, then sets a valid option. `tests/run-setsockopt-input.py` builds and runs that probe against `shim_cli`.

The focused probe and `./build.sh cli` passed. An isolated private Arch boot captured a fresh login prompt and settled D-Bus broker and launcher; the runner stopped only its guest and the exact disposable rootfs clone was removed.

This is a guest-memory boundary, not a claim that every Linux socket option has Darwin-equivalent behavior. Option mapping and host socket semantics remain separate compatibility work.
