# ARM64 #29 ioctl FIONREAD guest output

Only the `FIONREAD` ioctl branch changes. It checks that the guest's
four-byte result is writable before querying the host fd, then copies the
answer through checked guest output. Invalid or read-only guest addresses
return `-EFAULT` rather than being dereferenced by the shim.

`python3 tests/run-fionread-memory.py` runs a disposable ARM64 guest with
a pipe. It checks malformed and read-only result pointers, the queued byte
count before and after a read, and that failed queries did not consume data.

Other ioctl requests and broader Linux device behavior are unchanged and
not covered by this test.
