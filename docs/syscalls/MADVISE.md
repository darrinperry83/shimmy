# madvise (ARM64 syscall 233)

The old handler returned success for every advice and every address. Shimmy
now checks Linux advice numbers, page alignment, length overflow, and full
guest mapping coverage. `MADV_NORMAL`, `MADV_RANDOM`, `MADV_SEQUENTIAL`, and
`MADV_WILLNEED` are passed to the corresponding nondestructive Darwin advice
after validation. Linux and Darwin assign those four values identically.

Known advice with data-discard, fork, dump, huge-page, guard, or other VMA
effects returns `EOPNOTSUPP` until the required Linux semantics are modeled.
Unknown advice returns `EINVAL`; an unmapped range returns `ENOMEM`. Valid
zero-length hints succeed without touching memory. This is a bounded subset,
not full Linux `madvise` compatibility.

Run `./build.sh cli` and `python3 tests/run-madvise.py`. The test builds a
disposable ARM64 guest and checks supported hints, unchanged data, unsupported
effects, invalid arguments, mapping holes, and unmap behavior.
