# `personality` (ARM64 syscall 92): default Linux subset

Shimmy previously returned success for every `personality()` request without applying the requested execution behavior. The bounded implementation now answers the Linux query sentinel `0xffffffff` with the old default personality, `PER_LINUX` (0), and accepts a reset to that same value. It rejects every other personality or flag with `EOPNOTSUPP` without changing the result of a later query.

This is deliberately narrower than Linux: the Linux syscall stores arbitrary 32-bit personality bits, which affect address layout, executable mapping, uname, and compatibility behavior elsewhere in the kernel. Shimmy has no coherent implementation of these side effects. In particular, accepting `ADDR_NO_RANDOMIZE` or `READ_IMPLIES_EXEC` and ignoring it would mislead a program about security-relevant memory behavior.

Run `python3 tests/run-personality.py --sysroot <installed Arch root>` for the disposable ARM64 guest test. It covers the query sentinel, default reset, rejected memory flags and alternate execution domain, and verifies that rejected settings do not mutate the query result. The guest test checks this bounded syscall contract, not full Linux personality support.

Primary sources: [Linux `kernel/exec_domain.c` syscall](https://github.com/torvalds/linux/blob/master/kernel/exec_domain.c), [Linux `include/linux/personality.h` setter](https://github.com/torvalds/linux/blob/master/include/linux/personality.h), and [Linux UAPI flags](https://github.com/torvalds/linux/blob/master/include/uapi/linux/personality.h).
