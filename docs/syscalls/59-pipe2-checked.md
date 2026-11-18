# ARM64 #59 `pipe2`: checked output and honest packet-mode refusal

`net.c` owns the effective `pipe2` dispatch. Previously it created host pipe
descriptors and wrote their numbers through an unchecked guest pointer. An
invalid or read-only guest address could fault the shim after descriptor
creation. The route now preflights the 8-byte guest output, stages both
numbers, and uses checked copyout; if that final copy fails it closes both
host descriptors. Normal pipe data flow and NONBLOCK/CLOEXEC flags remain.

Linux `O_DIRECT` requests packet-mode pipes. That behavior was silently
ignored and is now refused with `EOPNOTSUPP` before creating a pipe. Other
invalid flag bits retain `EINVAL`.

`tests/run-pipe2-checked.py` exercises valid flow and flags, invalid and
read-only output addresses, repeated invalid calls without descriptor leaks,
and unsupported flags in a disposable ARM64 guest. CLI build and private
Arch login/D-Bus boot are regression gates. Packet-mode pipes remain
unsupported.
