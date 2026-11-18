# ARM64 #132 `sigaltstack` shadow correction

The existing implementation records an alternate stack for libc queries. Shimmy
does not deliver guest signal handlers on that stack, so this is **not** full
Linux signal-stack support. This change keeps that boundary while fixing two
concrete ABI problems: unchecked guest pointers could crash the host, and a
process-global shadow let guest threads overwrite one another's state.

The shadow is now host-thread-local. Input `stack_t` is copied from checked
guest memory, and previous state is written through checked guest memory before
the new state is committed. A bad input or output pointer returns `EFAULT`
without changing the shadow. Existing supported flags (`SS_DISABLE` and the
Linux-compatible no-op input `SS_ONSTACK`) and minimum stack size remain as
before. `SS_AUTODISARM` remains unsupported. Fork/exec edge cases and actual
delivery on an alternate stack are outside this bounded correction.

Proof: `./build.sh cli`; `python3 tests/run-sigaltstack.py --sysroot
<Arch rootfs>` executes raw ARM64 pointer/flag/transaction checks and a
separate pthread isolation probe. The private Arch boot gate reached a fresh
login prompt with D-Bus settled. These tests do not claim signal-delivery
compatibility.
