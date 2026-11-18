# ARM64 #64 `write`: checked guest input

The prior route passed a raw guest pointer to Darwin `write` and read it again
from diagnostics. Guest ELF or executable mappings are not necessarily readable
through that host path. The route now copies a bounded request into host memory
before making one host `write`. An invalid guest pointer returns `EFAULT`
without output, including when diagnostics are enabled. Successful writes keep
the host's partial-write and fd-error results; a zero-length call still goes
to host `write` to preserve fd behavior. Input above 1 MiB is refused with
`EOPNOTSUPP`, not split into potentially different pipe or socket records.

`tests/run-write-memory.py` verifies stack, static and executable ELF input,
invalid pointers without partial output, over-bound refusal, zero-length fd
behavior, and diagnostics in a disposable ARM64 guest. A CLI build and private
Arch login/D-Bus boot are regression gates; this is not a claim of full Linux
write semantics for every fd type or arbitrarily large request.
