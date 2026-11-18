# ARM64 #33 mknodat checked pathname

The existing regular-file and FIFO subset now snapshots the bounded guest
pathname before path resolution or host creation. Invalid and unterminated
guest strings return `-EFAULT` or `-ENAMETOOLONG`; invalid file types retain
their existing precedence. The supported creation modes are unchanged.

`python3 tests/run-mknodat-path.py` uses a disposable ARM64 guest root. It
checks malformed/NULL/unterminated pathnames, a read-only executable static
path, invalid type precedence, actual regular-file and FIFO creation, and
`EEXIST` on repetition.

Device nodes and sockets remain unsupported; ownership, ACL and path races
are outside this bounded memory correction.
