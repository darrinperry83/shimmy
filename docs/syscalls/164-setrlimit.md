# ARM64 `setrlimit` (#164): bounded resource changes

The shim now copies the 16-byte Linux `rlimit` input through checked guest
memory, validates `soft <= hard`, and rejects unrepresentable finite values
instead of silently treating them as infinity. Linux resource numbers above
15 return `EINVAL`. Resources 10–15 return `EOPNOTSUPP` because Darwin has no
equivalent modeled here. Linux `RLIMIT_RSS` (5) is also refused: Darwin's
same-named resource aliases its address-space limit, so forwarding a set would
change the wrong limit. `RLIMIT_NOFILE` soft values above the currently
advertised 4096 guest descriptors are refused.

For mapped resources 0–4 and 6–9, the shim still calls Darwin's per-process
`setrlimit`. This supports a useful subset but does **not** provide separate
Linux namespaces, guest-only limit enforcement, or parity for all resource
effects. `prlimit64` has a separate implementation and can still set a NOFILE
soft value above 4096 while its query reports the cap; that cross-call gap
needs a shared guest limit model. No host-wide clock or system limit is set.

`python3 tests/run-setrlimit.py` builds a disposable ARM64 guest and checks a
supported no-op set, invalid guest pointer, invalid/unsupported resources,
inverted soft/hard limits, unrepresentable finite values, NOFILE cap, and
unchanged queried core limits after refusals. `./build.sh cli` and that probe
passed on 2026-11-18. A private Arch boot regression is delegated to the
integration batch gate.
