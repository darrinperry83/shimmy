#!/usr/bin/env python3
"""Refresh a static ARM64 dispatch/risk snapshot; never assert ABI conformance.

Usage: python3 docs/syscalls/audit_snapshot.py /path/to/asm-generic/unistd.h
The adjacent asm/unistd_64.h is the authoritative native AArch64 table;
the generic header identifies compat-only entries. No guest is run.
"""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from native_header import numbered_native_entries

repo = Path(__file__).resolve().parents[2]
header = Path(sys.argv[1]).resolve(strict=True)
native_header = header.parent.parent / "asm/unistd_64.h"
if header.parent.name != "asm-generic" or not native_header.is_file():
    raise SystemExit("expected asm-generic/unistd.h beside asm/unistd_64.h")
_, compat_only = numbered_native_entries(header.read_text())
with tempfile.TemporaryDirectory(prefix="shimmy-syscall-audit-") as tmp:
    raw = Path(tmp) / "dispatch.json"
    subprocess.run([sys.executable, str(repo / "docs/syscalls/inventory.py"),
                    str(repo), str(native_header), str(raw)], check=True,
                   stdout=subprocess.DEVNULL)
    ledger = json.loads(raw.read_text())

# These are source-review queues, not proofs of a bug in every supported form.
review = {
    22: "epoll_pwait rejects non-null guest sigmask; atomic temporary-mask behavior is absent",
    39: "umount2 returns unconditional success without unmounting",
    40: "mount accepts broad no-op success after a narrow /run/user exception",
    72: "pselect6 rejects non-null guest sigmask; atomic temporary-mask behavior is absent",
    73: "ppoll rejects non-null guest sigmask; atomic temporary-mask behavior is absent",
    140: "setpriority has bounded guest target handling; wider priority semantics need review",
    141: "getpriority has bounded guest target handling; wider priority semantics need review",
    151: "setfsuid returns current euid without changing fsuid",
    152: "setfsgid returns current egid without changing fsgid",
    154: "setpgid has bounded guest target handling; wider process-group semantics need review",
    155: "getpgid has bounded guest target handling; wider process-group semantics need review",
    156: "getsid has bounded guest target handling; wider session semantics need review",
    168: "getcpu has bounded virtual topology result; check cross-interface consistency",
    171: "adjtimex supports bounded read-only state; clock adjustment is unsupported",
    186: "SysV msgget uses host-global IPC namespace and guest key",
    187: "SysV msgctl uses host-global IPC identifier",
    188: "SysV msgrcv uses host-global IPC identifier",
    189: "SysV msgsnd uses host-global IPC identifier",
    190: "SysV semget uses host-global IPC namespace and guest key",
    191: "SysV semctl uses host-global IPC identifier",
    193: "SysV semop uses host-global IPC identifier",
    194: "SysV shmget uses host-global IPC namespace and guest key",
    195: "SysV shmctl uses host-global IPC identifier",
    196: "SysV shmat maps host-global IPC segment",
    197: "SysV shmdt detaches host-global IPC segment",
    428: "open_tree returns /tmp directory fd, not detached mount",
    429: "move_mount returns unconditional success without move",
    430: "fsopen returns unconnected host datagram socket, not fs context",
    431: "fsconfig returns unconditional success without configuration",
    432: "fsmount returns /tmp directory fd, not mount",
    433: "fspick returns unconnected host datagram socket, not fs context",
    442: "mount_setattr returns unconditional success without attribute change",
}

rows = []
for entry in ledger["syscalls"]:
    n = entry["number"]
    locations = entry["dispatch_locations"]
    rows.append({
        "number": n, "name": entry["name"],
        "dispatch": "case_label" if locations else "none",
        "locations": locations,
        "trap_override_requires_review": entry["trap_override"],
        "duplicate_cases": len(locations) > 1,
        "source_review_flag": review.get(n),
        "compatibility": "unverified",
    })

snapshot = {
    "schema_version": 1,
    "architecture": "aarch64-linux",
    "source_commit": ledger["source_commit"],
    "source_dirty_at_scan": bool(subprocess.check_output(
        ["git", "-C", str(repo), "status", "--porcelain", "--", "src"], text=True).strip()),
    "header_sha256": hashlib.sha256(native_header.read_bytes()).hexdigest(),
    "header_basename": str(native_header).split("/usr/include/", 1)[-1],
    "generic_header_sha256": hashlib.sha256(header.read_bytes()).hexdigest(),
    "generic_header_basename": str(header).split("/usr/include/", 1)[-1],
    "method": "installed native asm/unistd_64.h table, generic-header compat exclusion, inventory.py case-label scan, and manually reviewed source-risk flags; no guest ABI test",
    "excluded_compat_entries": [{"number": n, "name": name}
                                for n, name in sorted(compat_only.items())],
    "warning": "A dispatch case, test filename, or documented subset is not full Linux compatibility.",
    "counts": {
        "header_entries": len(rows),
        "excluded_compat_entries": len(compat_only),
        "case_labels": sum(row["dispatch"] == "case_label" for row in rows),
        "no_case_label": sum(row["dispatch"] == "none" for row in rows),
        "duplicate_case_numbers": sum(row["duplicate_cases"] for row in rows),
        "source_review_flags": sum(row["source_review_flag"] is not None for row in rows),
        "verified_full_compatibility": 0,
    },
    "syscalls": rows,
}
out = repo / "docs/syscalls/audit-2026-11-18.json"
out.write_text(json.dumps(snapshot, sort_keys=True, indent=2) + "\n")
print(out)
