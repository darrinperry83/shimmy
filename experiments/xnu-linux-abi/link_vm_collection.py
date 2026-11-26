#!/usr/bin/env python3
"""Link only the matched VM guest's driver set. Writes local out/vmapple only."""
import argparse
import json
from pathlib import Path
import re
import subprocess
from driver_gate import OUT, digest
from stock_collection import decode_collection, fileset_entries


def link(source):
    target = OUT / "vmapple"
    audit = json.loads((target / "kernel-audit.json").read_text())
    kernel = Path(audit["kernel"])
    if digest(kernel) != audit["sha256"] or kernel.name != "kernel.development.vmapple":
        raise ValueError("VM kernel audit is stale or targets another machine")
    kdk = Path("/Library/Developer/KDKs/KDK_26.5.1_25F80.kdk")
    control = kdk / "System/Library/Kernels/kernel.release.vmapple"
    uuids = subprocess.check_output(["xcrun", "dwarfdump", "--uuid", str(control)], text=True)
    expected_uuid = re.search(r"UUID: ([A-F0-9-]+) \(arm64e\)", uuids).group(1)
    decoded = decode_collection(source.read_bytes())
    stock_entries = fileset_entries(decoded)
    stock_uuid = next(x["uuid"] for x in stock_entries if x["identifier"] == "com.apple.kernel")
    if stock_uuid != expected_uuid:
        raise ValueError("guest stock kernel UUID does not match Apple's 25F80 VMAPPLE control")
    (target / "guest-stock.kc").write_bytes(decoded)
    expected = {x["identifier"] for x in stock_entries}
    roots = sorted(expected - {"com.apple.kernel"})
    collection = target / "vm-boot.kc"
    command = ["kmutil", "create", "--arch", "arm64e", "--variant-suffix", "development",
        "--new", "boot", "--strip", "none", "--kernel", str(kernel),
        "--boot-path", str(collection), "--no-variant-extension", "--explicit-only",
        "--repository", str(kdk / "System/Library/Extensions"), "--volume-root", "/",
        "--kdk", str(kdk), "--build", "25F80"]
    for identifier in roots:
        command += ["--bundle-identifier", identifier]
    (target / "collection-command.json").write_text(json.dumps(command, indent=2) + "\n")
    with (target / "collection-link.log").open("w") as stream:
        code = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT).returncode
    if code:
        raise RuntimeError(f"VM collection link failed ({code}); see {target / 'collection-link.log'}")
    entries = fileset_entries(collection.read_bytes())
    ids = {x["identifier"] for x in entries}
    uuid = next(x["uuid"] for x in entries if x["identifier"] == "com.apple.kernel")
    inspection = subprocess.run(["kmutil", "inspect", "--boot-path", str(collection),
        "--no-variant-extension", "--no-header"], text=True, capture_output=True)
    (target / "collection-inspection.txt").write_text(inspection.stdout + inspection.stderr)
    if ids != expected or uuid != audit["uuid"] or inspection.returncode:
        raise ValueError("VM collection IDs, kernel UUID or inspection failed")
    report = {"source": str(source), "source_sha256": digest(source),
        "stock_kernel_uuid": stock_uuid, "kernel_uuid": uuid, "kernel_sha256": audit["sha256"],
        "collection": str(collection), "sha256": digest(collection), "entries": entries,
        "exact_guest_entry_set": True, "collection_linked": True,
        "installed_in_guest": False, "modified_kernel_booted": False,
        "physical_driver_support_proven": False}
    (target / "collection-result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"VM_COLLECTION_LINK_PASS: {len(entries)} entries; kernel {uuid}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("guest_collection", type=Path)
    link(parser.parse_args().guest_collection)
