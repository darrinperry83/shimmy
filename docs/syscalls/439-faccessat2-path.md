# ARM64 #439 faccessat2 checked pathname

The handler now snapshots the bounded guest pathname before testing
`AT_EMPTY_PATH` or resolving it. Invalid and null guest addresses return
`-EFAULT` instead of being dereferenced or interpreted as an empty path.
The existing valid empty-string fd behavior and other access modes/flags
are unchanged.

`python3 tests/run-faccessat2-path.py` runs an ARM64 guest in a disposable
root. It checks malformed paths with and without `AT_EMPTY_PATH`, a
read-only executable pathname, and a valid empty string on an opened fd.

This is a guest-memory correction. It does not establish full Linux
`faccessat2` flag or permission semantics for every fd type.
