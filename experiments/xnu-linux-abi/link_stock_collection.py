#!/usr/bin/env python3
"""Link and inspect the complete stock driver set offline. Never installs it."""
import json
from pathlib import Path
import plistlib
import subprocess
from driver_gate import OUT, digest
from stock_collection import fileset_entries


def link():
    target = OUT / "driver-check"
    stock = json.loads((target / "stock-collection.json").read_text())
    inventory = json.loads((target / "stock-inventory.json").read_text())
    audit = json.loads((OUT / "kernel-audit.json").read_text())
    kernel = Path(audit["kernel"])
    if digest(kernel) != audit["sha256"]:
        raise ValueError("kernel audit is stale")
    kdk = Path("/Library/Developer/KDKs/KDK_26.5.1_25F80.kdk")
    info = kdk / "System/Library/CoreServices/SystemVersion.plist"
    if not info.exists() or plistlib.loads(info.read_bytes()).get("ProductBuildVersion") != "25F80":
        raise ValueError("install the matching 25F80 KDK before offline collection linking")
    roots = sorted(({x["identifier"] for x in stock["entries"]} |
                    {x["identifier"] for x in inventory["loaded_drivers"]}) - {"com.apple.kernel"})
    roots = [x for x in roots if not x.startswith("com.apple.kpi.")]
    expected = set(roots) | {"com.apple.kernel"}
    collection = target / "full-stock.kc"
    result_path = target / "full-stock-link-result.json"
    cache_key = {"kernel_sha256": audit["sha256"], "roots": roots,
                 "stock_collection_sha256": stock["extracted_sha256"], "build": "25F80"}
    cached = False
    if result_path.exists() and collection.exists():
        old = json.loads(result_path.read_text())
        cached = (old.get("inputs") == cache_key and old.get("returncode") == 0 and
                  old.get("sha256") == digest(collection))
    if not cached:
        command = ["kmutil", "create", "--arch", "arm64e", "--variant-suffix", "development",
            "--new", "boot", "--strip", "none", "--kernel", str(kernel), "--boot-path", str(collection),
            "--no-variant-extension", "--repository", str(kdk / "System/Library/Extensions"),
            "--volume-root", "/", "--kdk", str(kdk), "--build", "25F80", "--explicit-only"]
        for identifier in roots:
            command += ["--bundle-identifier", identifier]
        (target / "full-stock-command.json").write_text(json.dumps(command, indent=2) + "\n")
        with (target / "full-stock-link.log").open("w") as stream:
            code = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT).returncode
        if code:
            result_path.write_text(json.dumps({"inputs": cache_key, "returncode": code,
                "collection_linked": False}, indent=2) + "\n")
            raise RuntimeError(f"kmutil create failed ({code}); see out/driver-check/full-stock-link.log")
    entries = fileset_entries(collection.read_bytes())
    ids = {x["identifier"] for x in entries}
    missing = sorted(expected - ids)
    extra = sorted(ids - expected)
    kernel_uuid = next(x["uuid"] for x in entries if x["identifier"] == "com.apple.kernel")
    inspection = subprocess.run(["kmutil", "inspect", "--boot-path", str(collection),
                                "--no-variant-extension", "--no-header"], text=True, capture_output=True)
    (target / "full-stock-inspection.txt").write_text(inspection.stdout + inspection.stderr)
    if missing or extra or len(ids) != len(entries) or kernel_uuid != audit["uuid"] or inspection.returncode:
        raise ValueError("linked collection contents, kernel UUID, or inspection failed")
    report = {"inputs": cache_key, "returncode": 0, "collection": str(collection),
        "sha256": digest(collection), "kernel_uuid": kernel_uuid,
        "entries": entries, "stock_entry_count": len(stock["entries"]),
        "custom_entry_count": len(entries), "missing_ids": missing,
        "extra_ids": extra, "collection_linked": True,
        "runtime_driver_support_proven": False, "installed_as_boot_object": False}
    result_path.write_text(json.dumps(report, indent=2) + "\n")
    print(f"FULL_STOCK_COLLECTION_PASS: {len(entries)} entries, exact ID set; kernel {kernel_uuid}")
    return report


if __name__ == "__main__":
    link()
