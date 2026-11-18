# ARM64 `sched_setattr` (#274) checked guest input

The bounded `sched_attr` input is copied from guest memory before any host
priority change. The 4-byte size is checked first; the 48-byte v0 structure
and up to 4048 extension bytes are then copied with checked guest reads. A
bad address, including a structure or extension crossing into a protected
page, returns `EFAULT`. Existing scheduling policy, size, target and
single-thread limitations are preserved.

`tests/run-sched-setattr-input.py` runs a disposable raw ARM64 guest. It checks
an invalid address, a read-only unsupported-policy attribute, valid no-change
attribute, size bounds, protected base/extension pages, nonzero extension
refusal, zero extension acceptance, and unchanged nice value.

The focused probe and CLI build passed. The older
`tests/run-sched-setattr.py` fails at its optional nice-increase step on both
this branch and the published integration baseline (guest exit 11), so it is
not counted as a passing regression gate for this increment. A private Arch
boot reached a fresh login prompt with D-Bus settled; the exact guest and
rootfs clone were cleaned.
