# ARM64 `fsetxattr` (#7) checked guest input

The guest xattr name and complete value are copied to bounded host buffers
before descriptor-based mutation. Linux CREATE and REPLACE flags remain
mapped to Darwin xattr flags; unsupported or conflicting flags return `EINVAL`.
Values over Linux's 64 KiB maximum return `E2BIG`. Invalid guest addresses,
including a value that crosses into a protected page, return `EFAULT` before
any host xattr update.

`tests/run-fsetxattr-memory.py` creates a disposable file and runs a raw ARM64
guest. The probe checks an existing xattr, CREATE refusal, invalid name/value
and protected later page without mutation, flag/size bounds, successful
REPLACE using name/value in executable guest memory, and invalid-fd `EBADF`.
The adjacent #5 and #6 setter probes also passed.

The focused probe and CLI build passed. A private Arch boot reached a fresh
login prompt with D-Bus broker and launcher settled; the exact guest and
rootfs clone were cleaned. This increment does not establish full descriptor
ownership or xattr namespace compatibility for every guest fd type.
