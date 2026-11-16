# Isolated Arch greeter boot probe

This probe used a disposable copy-on-write Arch root under `build/greeter-root`.
It did not change the installed root or the running Shimmy session.

## Package and account setup

- Official Arch Linux ARM aarch64 packages: `greetd 0.10.3-2` and
  `greetd-agreety 0.10.3-2`.
- Package SHA-256: greetd
  `aca72b4dd09763e40558f4a0c2fa8141473aee9578edfb334f4c3c15c6d489e3`;
  agreety `6e8146f9a334cc01173754e1d1ffe5e425c5aca70503b5333bd94ab4c18f5657`.
- Both detached package signatures verified with `pacman-key --verify` as
  `Good signature from "Arch Linux ARM Build System <builder@archlinuxarm.org>"`
  with full trust. The test root used `LocalFileSigLevel = Required TrustedOnly`.
- `pacman -U` installed both packages in the clone. The package's sysusers hook
  originally failed on an unsupported Linux OFD lock. With the independently
  integrated OFD lock fix (`31a3ea2` on `development`),
  `systemd-sysusers /usr/lib/sysusers.d/greetd.conf` exited successfully and
  created the packaged `greeter` account (UID/GID 968). The earlier manual
  greeter account workaround was removed from the clone before this check.

## Service result

With `display-manager.service` linked to the packaged `greetd.service`, normal
systemd boot announced `Started Greeter daemon` and reached `Graphical
Interface`. There was no agreety login prompt. The guest journal showed the
first greetd exit as `ABRT`, then retries and a service start limit.

Direct invocation of the installed `/usr/bin/greetd` exposed the first blocker:

```text
thread 'main' panicked at greetd/src/main.rs:44:36:
unable to lock pages: ENOSYS
```

The syscall trace shows Linux `mlockall(3)`, i.e.
`MCL_CURRENT | MCL_FUTURE`. Native macOS `mlockall(3)` also returns `ENOSYS`;
native `mlock` on one mapped page succeeds but does not provide the
current-and-future process-wide guarantee greetd requests. Stock greetd
0.10.3 calls this unconditionally before starting its greeter or opening PAM;
no supported configuration switch to omit it was found in its source.

Shimmy must not report success for this call without enforcing the requested
current and future memory locking. A per-range `mlock` shim would require
complete coverage of guest mappings, stacks, heap growth and future allocation
failure semantics. That work was not attempted in this greeter probe. The
standard SDDM route is being evaluated separately for the first authenticated
desktop session. No greetd security setting or source was weakened.

## Evidence boundary

This establishes package authentication, real greeter account creation,
systemd service activation and the pre-PAM failure. It does not establish a
successful greeter login, `pam_systemd` session or desktop launch. The
throwaway `shimtest` credentials existed only in the disposable root and were
not used because greetd stopped before the prompt.
