# ARM64 #147 `setresuid`: saved-ID request in host-identity mode

Darwin has `setreuid`, not Linux `setresuid`. The previous host-identity
handler ignored the third argument and could return success for a saved UID
that was never installed. It now returns `EOPNOTSUPP` for any explicit saved
UID request, before changing the real or effective UID. The all-unchanged
`(-1, -1, -1)` call remains a no-op. The `RDUID0` guest-credential path is
unchanged.

This is deliberately narrower than Linux. Even requesting the current saved
UID explicitly is refused in host-identity mode, because the bridge cannot
promise saved-ID control through `setreuid`. Other real/effective UID changes
still rely on Darwin semantics; complete Linux credential and file-permission
parity is not claimed.

Proof: `./build.sh cli`; `python3 tests/run-setresuid-saved.py` compiles a
disposable ARM64 guest, checks refusal and all three IDs before/after without
changing any host credentials. A private Arch boot smoke reaches login with
D-Bus settled when run under the normal virtual-root configuration.
