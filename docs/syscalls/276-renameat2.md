# `renameat2` (ARM64 syscall 276)

The existing implementation maps ordinary rename to Darwin `renameat`,
`RENAME_NOREPLACE` to `RENAME_EXCL`, and `RENAME_EXCHANGE` to `RENAME_SWAP`.
This change validates Linux flags before touching either pathname: unknown
bits, unsupported `RENAME_WHITEOUT`, and the incompatible
`NOREPLACE|EXCHANGE` pair return `EINVAL`. Both guest paths are copied with
bounds and address checks before rootfs resolution, so invalid guest pointers
return `EFAULT` instead of crashing the host process. A copy is used for the
host operation, avoiding a second read of guest-controlled pathname memory.

The disposable ARM64 probe checks the invalid cases, existing-target
`NOREPLACE`, missing-target `EXCHANGE`, an actual atomic swap, and ordinary
rename. It checks the final host file contents after the guest exits. Darwin
filesystem support for `RENAME_EXCL` and `RENAME_SWAP` is still required;
cross-filesystem semantics, concurrent pathname replacement, and all rootfs
race boundaries have not been proved. This is a bounded compatibility step,
not a claim of full Linux rename behavior.

Verification on 2026-11-18: `./build.sh cli` passed; the disposable ARM64
`tests/run-renameat2.py` probe passed; an isolated private Arch boot reached a
fresh login prompt and settled both `dbus-broker` and `dbus-broker-launch`.
The boot harness stopped its guest, and its exact rootfs clone was removed.

Contracts: [Linux `renameat2` manual](https://man7.org/linux/man-pages/man2/renameat2.2.html)
and [XNU `renameatx_np` manual](https://www.newosxbook.com/src.php?file=%2Fbsd%2Fman%2Fman2%2Frename.2&tree=xnu).
