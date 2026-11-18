# `sync` (#81): host-wide side effect safeguard

Shimmy previously called Darwin `sync()` and returned success to the guest.
That operation can initiate writeback for unrelated macOS files and does
not provide a verified Linux `sync` completion guarantee for guest files.
The isolated candidate returns `ENOSYS` instead. This is an explicit safe
refusal, **not** Linux-compatible durability behavior. No host-wide sync is
invoked by the guest probe.

`./build.sh cli` and `tests/run-sync-refusal.py` pass. A separate private
Arch clone reached a fresh login prompt with D-Bus broker and launcher
settled; the harness stopped its exact guest and the clone was removed.
Full support needs guest-owned writable file and mapping tracking plus a
writeback contract that reports completion without flushing unrelated host
data or claiming stronger durability than Darwin provides.
