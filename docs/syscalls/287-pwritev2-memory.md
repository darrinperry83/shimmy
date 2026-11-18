# `pwritev2` (#287): bounded guest-memory bridge

The prior #287 implementation handled the native AArch64 offset ABI: x3 is the
full signed offset, x4 is ignored, -1 uses the current file position, and
nonzero RWF flags are refused before a write. It passed a sparse write above
4 GiB. Its remaining problem was passing the guest iovec pointer and guest
payload pointers directly to Darwin `writev`/`pwritev`.

This follow-up routes offset -1 through #66 `writev` and positioned calls
through #70 `pwritev`. Those handlers snapshot the iovec and payload using
checked guest reads before the host write. The dispatch retains the original
x3/x4 and flag handling. A bad vector address or payload returns `EFAULT`
without changing the disposable file. The focused raw AArch64 probe checks
those failures, a valid stack payload at both offsets, and unsupported flags.
The earlier `run-pwritev2.py` regression still checks the sparse high offset.
`./build.sh cli` and the isolated private Arch fresh LOGIN / settled D-Bus
boot gate passed.

This is a bounded subset. #66 refuses requests above 1 MiB; #70 caps a
positioned request at 1 MiB and refuses append and nonregular descriptors.
The focused probe did not establish all ELF/static payload cases: an
exploratory fixture returned `EFAULT` for executable/static constants through
both already-existing #66 and #70 paths, while the stack payload passed.
That shared guest-memory behavior needs separate diagnosis. Late unmapping
or concurrent mutation of guest memory after snapshot remains outside this
single-call guarantee. Nonzero RWF flags remain unsupported.
