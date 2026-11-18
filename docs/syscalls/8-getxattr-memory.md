# ARM64 #8 `getxattr`: checked pathname, name, and result

The effective path-based `getxattr` route passed raw guest pointers to the
host resolver and xattr API. Unlike the adjacent checked #10 `fgetxattr`
route, a bad path/name/value address could fault the shim, and executable
guest memory could be unreadable to the host API. This #8-only correction
snapshots bounded guest pathname and attribute name strings, resolves within
the guest root, stages up to 1 MiB of host value data, and copies only the
returned bytes through checked guest memory. A size-only query keeps its
existing behavior; a large declared output capacity can still retrieve a
small value. #9 `lgetxattr` remains a separate path.

`tests/run-getxattr-memory.py` checks executable-memory path/name input,
stack/static value output, query size, small-buffer `ERANGE` without output,
bad and read-only pointers, and a large declared capacity on a disposable
ARM64 guest file. CLI build and private Arch login/D-Bus boot are regression
gates. This is bounded pointer correctness, not full xattr family parity.
