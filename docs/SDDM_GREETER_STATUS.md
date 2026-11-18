# SDDM greeter probe (16 November 2026)

This is an isolated service probe, not a visible greeter or desktop-session result.
The installed Arch root already has `hyprland.desktop` and `plasma.desktop`
Wayland session entries and their commands. It does not have the SDDM package.

In a disposable copy-on-write Arch root, the cached official
`sddm-0.21.0-7-aarch64.pkg.tar.xz` and detached signature were checked with
`pacman-key --verify`: Good signature from Arch Linux ARM Build System at full
trust. Only that clone used `SigLevel = Required TrustedOnly DatabaseOptional`
and `LocalFileSigLevel = Required TrustedOnly`. `pacman -U` placed the SDDM
binary, service and `sddm` account in the clone, although some post-transaction
commands reported failure; do not treat this as a fully clean package operation.
The test enabled `display-manager.service` and `graphical.target` in the clone.

An isolated systemd boot reached `Started Simple Desktop Display Manager` and
`Graphical Interface`. SDDM logged `Initializing`, `Starting`, and `Logind
interface found`, but did not spawn Xorg, KWin, or a visible greeter during the
observation window. The installed SDDM default is `DisplayServer=x11`; applying
the separate `tests/fixtures/sddm-wayland.conf` test configuration did not yet
advance it. The public guest query
`loginctl show-seat seat0 -p CanTTY -p CanGraphical` returned:

```
CanTTY=yes
CanGraphical=no
```

Thus a logind seat exists, but it is not advertised as graphical. The virtual
DRM/udev seat representation was investigated separately. With the graphical
seat change now on `development` (commit `2e5eaaa`) applied to
this isolated branch, a fresh boot returned `CanGraphical=yes`. SDDM advanced:
it logged `Adding new display`, `Using VT 1`, `Display server started`, and
`Greeter starting`. Its helper opened a PAM session for the clone-only `sddm`
user (UID 962), then `pam_systemd` failed its `CreateSession` varlink call with
`Invalid argument`. The helper did launch KWin Wayland, but reported
`XDG_RUNTIME_DIR not set`; KWin reported `Could not determine the active
graphical session`, `Failed to find a free display socket`, and then faulted
in libkwin. This was a transient process, not a sustained greeter. The exact
socket bind was not observed; the missing runtime directory is consistent
with the failed logind session, but its cause is not independently proven.

The remaining milestone is a real logind-backed greeter session with a valid
runtime directory, sustained KWin and SDDM greeter processes, and then visual
frame and session-selection tests. Neither service startup nor transient KWin
launch proves a usable greeter, Plasma or Hyprland login, or GPU presentation.
No live root, Mac display, or existing session was changed.

## Integrated-development retest

A later isolated full boot used the then-current `development` CLI at
`bf28e6f` with its opt-in `RDPIDFD_SOCKET=1` and
`RDSOCKETOUTQ_COMPAT=1` features. The clone alone contained the SDDM Wayland
configuration. `seat0` reported `CanGraphical=yes`; SDDM reached `Greeter
starting`. In one aligned run, SDDM host PID 46339 and helper PID 46344 logged
the helper's PAM authentication returning. During a 45-second observation,
KWin never appeared, `loginctl` showed no SDDM session, and `/run/user/962`
did not exist. This run stopped **before** PAM session opening and is distinct
from the earlier transient-KWin run above.

A separate opt-in byte-count trace showed the helper successfully wrote its
HELLO (20 bytes) and AUTHENTICATED (24 bytes) to the SDDM auth Unix socket,
then waited for a reply. The parent did not log an auth-socket accept or
reply in that trace. SDDM v0.21.0's source confirms the helper waits for the
parent's environment and cookie reply at this point. The parent main thread
sample was parked in Shimmy's futex translation; a later aligned futex trace
showed indefinite `FUTEX_WAIT_BITSET|PRIVATE|CLOCK_REALTIME` on
`0x109551dc0`, with the word remaining zero and no wake during 25 seconds.
That is a real wait but does not itself prove a lost wake or identify the Qt
callsite. A standalone cross-process Unix listener/epoll check passed, so a
generic listener wakeup failure is not established. The current blocker is
the parent not progressing the SDDM helper authentication exchange; the
precise reason remains under investigation.

The later header-only D-Bus trace identified the direct wait as SDDM's
`DisplayManagerSeat` constructor calling `QDBusConnection::registerService`
again. The parent previously sent `RequestName` with serial 3. Its reply was
**not missing**: the broker coalesced `NameAcquired` and the 84-byte method
return (`reply_serial=3`) into one 279-byte recv; a first-frame-only diagnostic
had incorrectly missed it. SDDM owned the name and later D-Bus calls received
replies. At the final wait, its system-bus socket had no queued bytes. The
repeat call did not send another `RequestName` before parking. This narrows
the problem to Qt's repeated call path or ordering in this SDDM process; it
does not prove a broker delivery failure.

A tiny Qt6DBus AArch64 PIE executable in the disposable root registered
`org.freedesktop.DisplayManager` twice on the same guest bus successfully.
It also succeeded when the second registration ran from a pending-call
callback. A test that scheduled a callback before the first registration
returned still ran the callback *after* the first returned, so it did not
reproduce nested registration. These tests exclude a general inability to
register the same name twice but leave SDDM's particular ordering unresolved.
The separate eventfd counter experiment corrected a real Linux semantic gap
in a test branch; an isolated SDDM retest still showed no KWin process. It is
not the proven cause of the greeter stall and is not merged here.

## 18 November isolated GUI retest

At development `5983c9c`, both GUI and CLI rebuilt after a fresh pull. The
on-screen boot reached `Started Simple Desktop Display Manager`, and SDDM
reached `Greeter starting`, but no greeter KWin process appeared. The test
root had originally been populated by extracting the cached package rather
than completing its account setup. It contained `sddm` passwd and group
entries but lacked an `/etc/shadow` entry. In one traced run, the parent and
helper completed a 57-byte/688-byte auth-socket exchange, then
`user@962.service` exited `224/PAM`; the journal showed `unix_chkpwd` could
not obtain user information for `sddm`. This was a **fixture defect**, not a
demonstrated Shimmy PAM defect. The guest's own `systemd-sysusers` recreated
all three account records in the disposable root, and the SDDM probe now
rejects incomplete accounts before booting.

With the corrected account, the greeter still failed to appear. In another
run the parent had 44 unread helper-auth bytes while its main guest thread
waited in a futex and its Qt D-Bus thread waited in `ppoll`. A gated experiment
broadened the existing eventfd repoll condition; the eventfd receive queues
drained, but SDDM still did not launch KWin, so that source experiment was
reverted. The later helper journal reported PAM session opening, followed
50 seconds later by `pam_systemd` receiving `io.systemd.System` from
`CreateSession`. `systemd-logind` logged 25-second timeouts starting both
`user-runtime-dir@962.service` and `user@962.service`. These observations
identify the current session-service handoff as the next target; neither a
working greeter nor a repaired D-Bus/varlink exchange has been demonstrated.

A subsequent on-screen boot instrumented SDDM's own host `poll()` calls. Its
Qt D-Bus thread repeatedly received `POLLIN` on both the shim eventfd (fd 5)
and the connected system bus (fd 6). At the same time, the helper's 44-byte
authentication message remained queued on the parent auth socket and no KWin
process appeared. This rules out an indefinitely missed native `poll()` wake
as the sole cause of that run's stall. The instrumented `src/net.c` code was
reverted after the trace; this is diagnostic evidence, not a greeter fix.

In a separate disposable-root A/B boot, `dbus.service` launched the root's
installed `dbus-daemon --system --nofork --nopidfile --address=systemd:` in
place of `dbus-broker-launch`. The daemon stayed up, but SDDM again left its
44-byte helper authentication message unread and never spawned KWin during
the observation. The clone-only service override was removed afterward.
This does not prove the D-Bus exchange is correct, but replacing the broker
alone does not resolve the observed SDDM parent wait.

## Repeat-registration isolation and next gate

SDDM 0.21.0 [calls `QDBusConnection::registerService`](https://github.com/sddm/sddm/blob/v0.21.0/src/daemon/DisplayManager.cpp)
in its display-manager, seat, and session constructors. A temporary aarch64 `LD_PRELOAD` probe was
compiled outside the repository and used only by the disposable Arch clone.
It logged two calls from the **same SDDM parent PID**, at executable offsets
`0x5e85c` and `0x5e648`. Forwarding the first call and returning success from
only the repeat call let the parent drain the previously queued 44-byte helper
authentication message and send its reply. Without that bypass, the parent
remained in a futex wait and left those bytes unread. This is a causal
diagnostic for the SDDM parent wait, not a suitable Shimmy or package fix;
the interposer was removed. The proper fix must preserve normal Qt/D-Bus
behavior for unmodified packages.

The same bypass boot exposed a second gate. PAM opened the `sddm` user's
session, then logind timed out after 25 seconds each while starting
`user-runtime-dir@962.service` and `user@962.service`. It returned
`io.systemd.System` from CreateSession. The greeter KWin process then started
without `XDG_RUNTIME_DIR` and exited because it could not create a Wayland
socket. In an isolated debug rerun, PID 1's guest thread was parked in
`sys_epoll_wait_ts`/`kevent` during the stalled handoff, and no
`systemd-user-runtime-dir` helper was observed in the process inventory.
That does not yet establish whether the start request was dropped, delayed,
or rejected before reaching PID 1. The logind-to-manager request/reply is the
next trace target. The clone-only interposer and service overrides were
removed; no live root or session was changed.

An A/B boot masked the clone's noisy `sshd.service`, which had been logging
unsupported masked `ppoll` calls and inflating the test journal. SDDM still
stalled with 44 bytes queued, so SSH log volume alone did not cause the parent
wait. The SSH mask was removed from the clone afterward.

## 18 November: greeter reaches DRM, then rejects logind's handed card

In a later disposable-root on-screen boot, clone-only diagnostic overrides
skipped SDDM's second D-Bus registration call and prestarted
`user-runtime-dir@962.service`. A clone-only `busctl StartUnit` probe also
succeeded. Logind created a real `sddm` greeter session and KWin started, so
this run advanced beyond the earlier PAM/Wayland-socket failure. These
overrides are diagnostic and are **not** a normal greeter startup fix.

With `RDDRM_TRACE=1`, fd identity and peer-path logging showed that
`systemd-logind` opened the card (peer `drm-event-<logind-pid>-0`) and KWin
received that same open description as fd 23. KWin opened a new card fd,
obtained a nonzero magic token, then called `AUTH_MAGIC` on the handed fd.
Shimmy returned `EACCES` because KWin's process-local `g_master_owner_fd`
was unset. A test-only attempt to claim the session master in KWin returned
`EBUSY`: another process already held `drm-master.lock`. It was reverted;
blindly claiming a second master would violate the existing exclusive-lock
and host frame-authentication design. The proper integration must preserve
or broker the existing master capability across logind's `SCM_RIGHTS` fd
handoff, and must still reject an unrelated card fd or producer. The last
on-screen run did not display a usable greeter.
