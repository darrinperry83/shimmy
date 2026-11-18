#!/usr/bin/env python3
"""Static, reviewable ARM64 syscall dispatch inventory (not a correctness test)."""
import pathlib
import re
import sys
import hashlib
import json
import subprocess
from native_header import numbered_native_entries

repo = pathlib.Path(sys.argv[1]).resolve()
header = pathlib.Path(sys.argv[2]).resolve()
official, compat_only = numbered_native_entries(header.read_text())

def strip(text):
    # Retain character offsets for later extraction.
    return re.sub(r"/\*.*?\*/|//[^\n]*|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'",
                  lambda m: re.sub(r"[^\n]", " ", m.group()), text, flags=re.S)

def cases(src, fn):
    raw = src.read_text()
    text = strip(raw)
    start = text.index(fn + "(")
    switch = re.search(r"\bswitch\s*\(\s*nr\s*\)\s*\{", text[start:])
    assert switch, (src, fn)
    brace = start + switch.end() - 1
    depth = 1
    labels = []
    end = brace + 1
    for pos in range(brace + 1, len(text)):
        ch = text[pos]
        if ch == "{": depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                end = pos
                break
        if depth == 1 and text.startswith("case ", pos):
            m = re.match(r"case\s+(\w+)\s*:", text[pos:])
            if m: labels.append((m.group(1), pos, pos + m.end()))
    return raw, labels, end

net_raw = (repo / "src/net.c").read_text()
net_constants = {k: int(v) for k, v in re.findall(r"\b(LSYS_\w+)\s*=\s*(\d+)", net_raw)}
dispatched = {}
bodies = {}
for filename, function, tier in [
    ("syscalls.c", "linux_syscall", "main"),
    ("syscalls_ext.c", "linux_syscall_ext", "extended"),
    ("net.c", "net_syscall", "network"),
]:
    raw, labels, end = cases(repo / "src" / filename, function)
    pending = []
    for i, (label, pos, after) in enumerate(labels):
        nr = int(label) if label.isdecimal() else net_constants.get(label)
        # Consecutive labels share one body. A block is deliberately classified
        # conservatively; nested branches may have different semantics.
        body = raw[after:labels[i + 1][1] if i + 1 < len(labels) else end]
        if nr is not None and nr in official:
            pending.append((nr, pos))
        if not strip(body).strip():
            continue
        if re.search(r"return\s+-LX_ENOSYS\s*;", body): kind = "explicit ENOSYS or conditional"
        elif re.search(r"return\s+-LX_EPERM\s*;|return\s+-LX_EOPNOTSUPP\s*;", body): kind = "restricted or conditional"
        elif re.search(r"return\s+0\s*;", body): kind = "implemented or no-op (review)"
        else: kind = "dispatched (semantics unverified)"
        for grouped_nr, grouped_pos in pending:
            dispatched.setdefault(grouped_nr, []).append((tier, kind, raw.count("\n", 0, grouped_pos) + 1))
            bodies.setdefault(grouped_nr, []).append((tier, strip(body).strip()))
        pending.clear()

all_nrs = set(official)
print(f"Header unique numbered entries: {len(all_nrs)}; highest: {max(all_nrs)}")
print(f"Excluded compat-only numbered entries: {len(compat_only)}")
print(f"Dispatched by source case labels: {len(set(dispatched) & all_nrs)}")
print(f"No dispatch case: {len(all_nrs - set(dispatched))}")
print("No dispatch case list:")
for nr in sorted(all_nrs - set(dispatched)):
    print(f"  {nr:3} {official[nr]}")
print("Explicit unsupported or ambiguous cases:")
for nr in sorted(dispatched):
    if nr in official and any(kind != "dispatched (semantics unverified)" for _, kind, _ in dispatched[nr]):
        print(f"  {nr:3} {official[nr]}: " + ", ".join(f"{tier}:{kind}:{line}" for tier, kind, line in dispatched[nr]))

if len(sys.argv) > 3:
    notes = {
        29: "ioctl covers many synthetic device contracts; per-device conformance is required",
        39: "umount2 currently reports success without mount-state change",
        40: "mount currently reports success without mount-state change; tmp.mount subsequently fails",
        116: "syslog currently has a documented stub success",
        198: "socket family/protocol behavior varies; netlink udev boot gap under active work",
        220: "clone is handled in the shim trap handler, which needs the guest register file",
        221: "execve re-execs the host shim; verify file descriptors, signals and credentials",
        293: "rseq returns ENOSYS; guest libc may fall back",
        428: "open_tree returns a host directory fd, not a Linux detached mount",
        429: "move_mount is a no-op success",
        430: "fsopen returns an unconnected host datagram socket, not a filesystem context",
        431: "fsconfig is a no-op success",
        432: "fsmount returns a host directory fd, not a mounted filesystem",
        435: "clone3 has trap-handler support for some forms; other forms fail",
        442: "mount_setattr is a no-op success",
    }
    known_placeholders = {28, 30, 31, 39, 40, 116, 235, 237, 429, 431, 440, 442}
    boot_priority = {29, 39, 40, 57, 63, 64, 78, 79, 80, 93, 94, 95, 98, 198, 200,
                     203, 208, 209, 211, 212, 220, 221, 242, 278, 428, 429, 430,
                     431, 432, 433, 434, 435, 436, 437, 439, 442, 452}
    common_priority = {19, 20, 21, 22, 26, 27, 28, 67, 68, 72, 73, 74, 75, 76,
                       77, 99, 100, 101, 107, 108, 109, 110, 111, 118, 119,
                       120, 121, 122, 123, 127, 130, 131, 134, 135, 136, 137,
                       138, 139, 140, 141, 147, 148, 149, 150, 160, 163,
                       164, 165, 169, 172, 173, 174, 175, 176, 177, 178, 179,
                       180, 181, 182, 183, 184, 185, 186, 187, 188, 189,
                       190, 191, 192, 193, 194, 195, 196, 197, 199, 201,
                       202, 204, 205, 206, 207, 210, 213, 214, 215, 216,
                       217, 218, 219, 222, 223, 233, 234, 243, 260, 261,
                       262, 263, 264, 265, 266, 267, 268, 269, 270, 271,
                       272, 273, 274, 275, 276, 277, 280, 281, 282, 283,
                       284, 285, 286, 287, 288, 289, 290, 291, 292, 293,
                       441, 444, 445, 446, 449, 454, 455, 456}

    entries = []
    for nr, name in sorted(official.items()):
        locations = [
            {"tier": tier, "file": "src/" + {"main": "syscalls.c", "extended": "syscalls_ext.c", "network": "net.c"}[tier], "line": line}
            for tier, _, line in dispatched.get(nr, [])
        ]
        blocks = [body for _, body in bodies.get(nr, [])]
        direct_errors = [bool(re.fullmatch(r"return\s+-(?:LX_)?(?:ENOSYS|EPERM|EOPNOTSUPP)\s*;", b)) for b in blocks]
        if not locations:
            semantic = "unsupported"
            reason = "no dispatch case; falls through to extended default ENOSYS"
        elif nr in known_placeholders:
            semantic = "placeholder"
            reason = "source explicitly has a no-op or synthetic success; verify the observable contract"
        elif blocks and all(direct_errors):
            semantic = "unsupported"
            reason = "direct unsupported error return"
        elif any(re.search(r"return\s+-(?:LX_)?(?:ENOSYS|EPERM|EOPNOTSUPP)\s*;", b) for b in blocks):
            semantic = "partial_or_unknown"
            reason = "contains an unsupported branch; supported subset not proven"
        else:
            semantic = "unknown"
            reason = "dispatch present; Linux semantics not verified by this static scan"
        if nr in (220, 435):
            semantic = "partial_or_unknown"
            reason = "trap handler also handles this syscall; case body alone is insufficient"
        entries.append({
            "number": nr, "name": name,
            "dispatch_state": "case" if locations else "none",
            "dispatch_locations": locations,
            "trap_override": nr in (220, 435),
            "semantic_status": semantic,
            "classification_reason": reason,
            "verified_compatible": False,
            "test_evidence": [],
            "runtime_evidence": [],
            "priority": "P0_boot" if nr in boot_priority else ("P1_common" if nr in common_priority else "P2_other"),
            "notes": notes.get(nr, ""),
        })
    counts = {status: sum(e["semantic_status"] == status for e in entries)
              for status in ("unknown", "partial_or_unknown", "placeholder", "unsupported")}
    try:
        commit = subprocess.check_output(["git", "-C", str(repo), "rev-parse", "HEAD"], text=True).strip()
        dirty = bool(subprocess.check_output(["git", "-C", str(repo), "status", "--porcelain"], text=True).strip())
    except (subprocess.CalledProcessError, FileNotFoundError):
        commit, dirty = None, None
    ledger = {
        "schema_version": 1,
        "architecture": "aarch64-linux",
        "source_repo": repo.name,
        "source_commit": commit,
        "source_dirty_at_scan": dirty,
        "header": sys.argv[2],
        "header_sha256": hashlib.sha256(header.read_bytes()).hexdigest(),
        "excluded_compat_entries": [{"number": nr, "name": name}
                                    for nr, name in sorted(compat_only.items())],
        "method": "static case-label scan; no syscall or integration test was run",
        "counts": {"numbered_header_entries": len(entries),
                   "excluded_compat_entries": len(compat_only),
                   "has_dispatch_case": len(dispatched),
                   "no_dispatch_case": len(entries) - len(dispatched), **counts},
        "syscalls": entries,
    }
    pathlib.Path(sys.argv[3]).write_text(json.dumps(ledger, indent=2, sort_keys=True) + "\n")
