#!/usr/bin/env python3
"""Stage a reviewable kernel/firmware reference bundle locally, without installing."""
import json
from pathlib import Path
import shutil
import hashlib
from driver_gate import OUT, digest
from stock_inventory import im4p_object


def main():
    reports = OUT / "driver-check"
    gate = json.loads((reports / "preboot-report.json").read_text())
    if not gate["driver_preboot_gate_passed"]:
        raise ValueError("driver pre-boot gate has not passed")
    link = gate["collection"]
    kc = Path(link["collection"])
    if digest(kc) != link["sha256"]:
        raise ValueError("linked kernel collection changed after inspection")
    stock = json.loads((reports / "stock-inventory.json").read_text())
    if digest(Path(stock["shared_sep"]["path"])) != stock["shared_sep"]["sha256"]:
        raise ValueError("shared SEP reference changed after inventory")
    for component in stock["active_fud_components"]:
        path = Path(component["active_file"])
        if (digest(path) != component["container_sha256"] or
            hashlib.sha384(im4p_object(path.read_bytes())).hexdigest() != component["manifest_sha384"]):
            raise ValueError(f"firmware changed: {component['component']}")
    boot = Path(stock["boot_objects"])
    sources = [(kc, "kernel.development.t6020.kc")]
    for path in sorted((boot / "usr/standalone/firmware/FUD").glob("*.img4")):
        sources.append((path, f"firmware/FUD/{path.name}"))
    sources += [(boot / "usr/standalone/firmware/devicetree.img4", "firmware/devicetree.img4"),
                (Path(stock["shared_sep"]["path"]), "firmware/shared-sep-firmware.img4")]
    destination = OUT / "boot-candidates" / link["sha256"][:16]
    files = []
    # Verify every source before creating the bundle. No policy/ticket files are copied.
    for source, relative in sources:
        if not source.is_file():
            raise ValueError(f"missing candidate input: {source}")
        files.append({"source": str(source), "path": relative,
                      "sha256": digest(source), "size": source.stat().st_size})
    for item in files:
        target = destination / item["path"]
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.exists():
            if digest(target) != item["sha256"]:
                raise ValueError(f"refusing changed candidate file: {target}")
        else:
            shutil.copy2(item["source"], target)
        if digest(target) != item["sha256"]:
            raise ValueError(f"candidate copy verification failed: {target}")
    manifest = {"model": stock["model"], "board": stock["board"], "build": stock["product_build"],
        "kernel_uuid": link["kernel_uuid"], "collection_sha256": link["sha256"],
        "stock_collection_entry_count": link["custom_entry_count"], "files": files,
        "required_probe_boot_argument": "rdxnu_linux_probe=1",
        "retain_stock_runtime_firmware_root": stock["on_demand_firmware_root"],
        "retain_stock_driverkit_and_darwin_services": True,
        "on_demand_firmware_file_count": len(stock["on_demand_firmware_files"]),
        "driverkit_extension_count": len(stock["installed_driverkit_extensions"]),
        "firmware_containers_are_unchanged_stock_references": True,
        "encoded_or_personalized_for_custom_boot": False,
        "bootloader_handoff_proven": False, "installed": False, "booted": False}
    (destination / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (destination / "README.txt").write_text(
        "LOCAL PREFLIGHT BUNDLE: NOT AN INSTALLED OR PERSONALIZED BOOT OBJECT\n\n"
        "Contains the custom full-stock kernel collection and unchanged stock firmware references.\n"
        "The firmware containers retain their original stock personalization; they have not been\n"
        "prepared for a new boot identity. No trust policy, boot ticket, or startup disk was changed.\n"
        "Keep the matching stock Darwin root, services, DriverKit and runtime firmware trees.\n"
        "Boot encoding/personalization, the alternate boot target and loader handoff remain work.\n"
        "No runtime hardware or Linux syscall execution is established by this bundle.\n")
    print(f"CANDIDATE_STAGED: {destination}; {len(files)} copied files verified")


if __name__ == "__main__":
    main()
