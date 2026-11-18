# `futex_requeue` (AArch64 456)

Shimmy implements the **private U32, zero-requeue** subset of Linux's four-argument futex2 call. Two `struct futex_waitv` entries are checked, including address, size, private flag, value width, and reserved fields. With `nr_requeue == 0`, the source futex word is compared under the source queue lock and up to `nr_wake` waiters are woken. The result is the number woken.

Actual movement of sleeping waiters to another address returns `EOPNOTSUPP`. The current queue stores stack waiters under a fixed bucket and waits on that bucket's condition variable; changing only a waiter's address would strand it. Shared and NUMA futexes are not supported. This does not claim full futex2 compatibility.

`python3 tests/run-futex-requeue2.py` compiles a static ARM64 guest and checks validation, source comparison, no-waiter return, and waking a real waiter. `./build.sh cli` is the integration build gate.

ABI and semantics: [Linux syscall entry](https://codebrowser.dev/linux/linux/kernel/futex/syscalls.c.html#442), [Linux requeue implementation](https://codebrowser.dev/linux/linux/kernel/futex/requeue.c.html#379), [UAPI waiter layout](https://codebrowser.dev/linux/linux/include/uapi/linux/futex.h.html#95).
