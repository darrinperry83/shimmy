#!/usr/bin/env python3
"""Install the evidence-based SEP base interfaces into the disposable XNU tree."""
import hashlib
import json
from pathlib import Path
import subprocess
import re
import prepare as source_pin

HERE = Path(__file__).resolve().parent


def prepare(root):
    commit = subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD"], text=True).strip()
    if commit != source_pin.COMMIT:
        raise ValueError("unexpected XNU revision for SEP interface reconstruction")
    receipt_path = root / ".rdxnu-sep-source.json"
    receipt = json.loads(receipt_path.read_text()) if receipt_path.exists() else {}
    updates = {}
    for source, destination in (("kernel/IOKit/SEPHibernator.h", "iokit/IOKit/SEPHibernator.h"),
                                ("kernel/SEPHibernator.cpp", "iokit/Kernel/SEPHibernator.cpp")):
        target = root / destination
        content = (HERE / source).read_bytes()
        if target.exists() and target.read_bytes() != content:
            if hashlib.sha256(target.read_bytes()).hexdigest() != receipt.get(destination):
                raise ValueError(f"refusing foreign edits in {target}")
        updates[destination] = content
    exports = (HERE / "kernel/sep.exports").read_text()
    names = exports.splitlines()
    if len(set(names)) != len(names) or not all(re.fullmatch(r"__[A-Za-z0-9_]+", name) for name in names):
        raise ValueError("invalid SEP export list; comments/whitespace can become linker aliases")
    for destination, addition in (
        ("iokit/conf/files", "\n# RDXNU: SEP base interfaces required by the stock driver\n"
                             "iokit/Kernel/SEPHibernator.cpp optional iokitcpp\n"),
        ("config/IOKit.arm64.MacOSX.exports", "\n" + exports),
    ):
        original = subprocess.check_output(["git", "-C", str(root), "show", f"HEAD:{destination}"])
        content = original + addition.encode()
        current = (root / destination).read_bytes()
        if current not in (original, content) and hashlib.sha256(current).hexdigest() != receipt.get(destination):
            raise ValueError(f"refusing foreign edits in {destination}")
        updates[destination] = content
    for destination, content in updates.items():
        target = root / destination
        if not target.exists() or target.read_bytes() != content:
            target.write_bytes(content)
    receipt_path.write_text(json.dumps({name: hashlib.sha256(data).hexdigest()
        for name, data in updates.items()}, indent=2) + "\n")


if __name__ == "__main__":
    import sys
    prepare(Path(sys.argv[1]).resolve())
