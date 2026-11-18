# ARM64 #25 fcntl lock guest memory

The `F_GETLK`, `F_SETLK`, `F_SETLKW` and corresponding OFD lock commands
now snapshot the 32-byte Linux `flock` through checked guest copy-in.
GETLK variants preflight their output range and copy the result back through
checked copyout. Invalid, read-only or partly unreadable guest structures
return `-EFAULT` instead of directly dereferencing the pointer or invoking
the host lock operation.

`python3 tests/run-fcntl-lock-memory.py` uses a disposable ARM64 guest file
and checks invalid pointers, a read-only GETLK destination, an input struct
crossing into a protected page, and a real SETLK/GETLK sequence.
The existing `tests/guest_ofd_locks.c` guest probe also passes against this
build, covering an OFD lock's duplicate-descriptor lifetime and contention.

This corrects guest memory handling. Linux lock ownership, contention,
OFD inheritance and host filesystem parity require their separate tests.
