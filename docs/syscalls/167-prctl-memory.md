# ARM64 #167 prctl guest-memory subset

`PR_SET_NAME` now stages up to 15 checked guest bytes before changing the
shim's existing process-name state. The name may fill the field without a
NUL terminator. A later unreadable byte returns `-EFAULT` and leaves the
old name intact. `PR_GET_NAME` and `PR_GET_PDEATHSIG` write through checked
guest copyout, rejecting invalid or read-only destinations.

`python3 tests/run-prctl-memory.py` uses a disposable ARM64 guest to check
invalid SET addresses, a name crossing into a protected page, old-name
preservation, executable static names including 15 bytes without NUL, and
GET output boundaries.

This is only a guest-memory correction for those three options. The
existing broader `prctl` option semantics, including process-versus-thread
name state, remain separate compatibility work.
