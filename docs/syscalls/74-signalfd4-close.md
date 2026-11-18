# ARM64 #74 `signalfd4`: retire guest-owned read fd on close

The signalfd backend reserves both ends of its host socket pair so a vfork
child sharing the host descriptor table cannot close its parent's signalfd.
The core `close` route previously treated every reserved fd as shim-private;
it reported success for an ordinary guest's signalfd close without retiring
the slot, its writer, or the read fd. Repeated create/close exhausted the
16-slot backend.

The close route now asks the signalfd backend to retire **only an active
signalfd read fd** when the caller is not a vfork child. The backend clears
the slot, unreserves the read fd, and closes/unreserves its private writer.
The core removes the read fd from epoll and then closes it. Other reserved
descriptors and a vfork child's close keep their existing protection.

`tests/run-signalfd4-close.py` builds a disposable ARM64 guest that checks
create, epoll registration, close, stale update/read failure, epoll removal,
and 20 further create/close cycles. The existing host signalfd test covers
record delivery and backend close. A private Arch login/D-Bus boot is a
startup regression gate, not proof of complete Linux signalfd semantics.

This is a close(2) lifecycle correction. Duplicated signalfd descriptors and
close_range behavior still need a separately designed shared-owner model;
this change does not claim that compatibility.
