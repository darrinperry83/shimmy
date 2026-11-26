#!/usr/bin/env python3
"""Inventory the running stock hardware stack; hash firmware without changing it."""
import hashlib
import json
from pathlib import Path
import plistlib
import re
import subprocess

HERE = Path(__file__).resolve().parent
OUT = HERE / "out"


def capture(*args):
    return subprocess.check_output(list(map(str, args)))


def file_hash(path, algorithm="sha256"):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, algorithm).hexdigest()


def der_node(data, offset, limit):
    if offset + 2 > limit:
        raise ValueError("truncated DER node")
    tag, size = data[offset:offset + 2]
    start = offset + 2
    if size & 0x80:
        count = size & 0x7f
        if not count or count > 8 or start + count > limit:
            raise ValueError("invalid DER length")
        size = int.from_bytes(data[start:start + count], "big")
        start += count
    if start + size > limit:
        raise ValueError("DER node exceeds container")
    return tag, start, start + size


def im4p_object(data):
    tag, start, end = der_node(data, 0, len(data))
    if tag != 0x30 or end != len(data):
        raise ValueError("expected one complete IMG4/IM4P sequence")
    tag, a, b = der_node(data, start, end)
    if tag != 0x16:
        raise ValueError("missing Image4 container name")
    if data[a:b] == b"IM4P":
        return data
    if data[a:b] != b"IMG4":
        raise ValueError("unsupported Image4 container")
    position = b
    while position < end:
        tag, a, b = der_node(data, position, end)
        if tag == 0x30:
            child_tag, c, d = der_node(data, a, b)
            if child_tag == 0x16 and data[c:d] == b"IM4P":
                return data[position:b]
        position = b
    raise ValueError("IMG4 contains no IM4P payload")


def loaded_drivers(text):
    entries = []
    pattern = re.compile(r"\b(com\.apple\.\S+)\s+\(([^)]+)\)\s+([A-F0-9-]{36})\s+<([^>]*)>")
    for line in text.splitlines():
        match = pattern.search(line)
        if match:
            entries.append({"identifier": match[1], "version": match[2], "uuid": match[3]})
    if not entries or len({x["identifier"] for x in entries}) != len(entries):
        raise ValueError("empty or ambiguous loaded-driver inventory")
    return entries


def main():
    build = capture("sw_vers", "-buildVersion").decode().strip()
    expected = json.loads((HERE / "dependencies.json").read_text())["release"]
    if f"({build})" not in expected:
        raise ValueError("running OS does not match the pinned driver/KDK build")
    model = capture("sysctl", "-n", "hw.model").decode().strip()
    platform = plistlib.loads(capture("ioreg", "-a", "-rd1", "-c", "IOPlatformExpertDevice"))[0]
    compatible = platform["compatible"]
    if isinstance(compatible, bytes):
        compatible = compatible.decode().split("\0")
    elif isinstance(compatible, str):
        compatible = [compatible]
    board = next(x.lower() for x in compatible if re.fullmatch(r"[Jj][A-Za-z0-9]+AP", x))
    chosen = plistlib.loads(capture("ioreg", "-a", "-p", "IODeviceTree", "-r", "-n", "chosen", "-d", "1"))[0]
    object_path = chosen["boot-objects-path"]
    if isinstance(object_path, bytes):
        object_path = object_path.rstrip(b"\0").decode()
    match = re.fullmatch(r"/([A-F0-9-]{36})/boot/([A-F0-9]{96})", object_path)
    if not match:
        raise ValueError("unexpected boot-object path")
    if chosen["boot-manifest-hash"].hex().upper() != match[2]:
        raise ValueError("chosen boot path and boot-manifest hash disagree")
    volume = Path("/System/Volumes/Preboot") / match[1]
    boot = volume / "boot" / match[2]
    manifest_path = volume / "restore/BuildManifest.plist"
    manifest = plistlib.loads(manifest_path.read_bytes())
    if manifest.get("ProductBuildVersion") != build:
        raise ValueError("restore manifest does not match the running OS build")
    identities = [b for b in manifest["BuildIdentities"] if
                  b.get("Info", {}).get("DeviceClass", "").lower() == board and
                  b["Info"].get("RestoreBehavior") == "Update"]
    if len(identities) != 1:
        raise ValueError("expected exactly one board/update identity")
    identity = identities[0]
    firmware = []
    for name, component in identity["Manifest"].items():
        info = component.get("Info", {})
        if not info.get("IsFUDFirmware") or not info.get("Path", "").endswith(".im4p"):
            continue
        aliases = [name, name.removeprefix("Ap,")]
        if name == "Ap,DCP2":
            aliases.append("DCP")
        files = {boot / "usr/standalone/firmware/FUD" / f"{alias}.img4" for alias in aliases}
        candidates = sorted(p for p in files if p.is_file())
        entry = {"component": name, "manifest_path": info["Path"],
                 "loaded_by_iboot_declared": bool(info.get("IsLoadedByiBoot")),
                 "manifest_sha384": component.get("Digest", b"").hex(),
                 "present": len(candidates) == 1, "im4p_digest_matches": False}
        if len(candidates) == 1:
            path = candidates[0]
            payload = im4p_object(path.read_bytes())
            entry.update(active_file=str(path), container_sha256=file_hash(path),
                         im4p_sha384=hashlib.sha384(payload).hexdigest())
            entry["im4p_digest_matches"] = entry["im4p_sha384"] == entry["manifest_sha384"]
        firmware.append(entry)
    resources = []
    resource_root = Path("/usr/share/firmware")
    for path in sorted(resource_root.rglob("*")):
        if path.is_file():
            resources.append({"path": str(path.relative_to(resource_root)),
                              "size": path.stat().st_size, "sha256": file_hash(path)})
    dexts = []
    for path in sorted(Path("/System/Library/DriverExtensions").glob("*.dext")):
        info_path = path / "Info.plist"
        if not info_path.exists():
            info_path = path / "Contents/Info.plist"
        if info_path.exists():
            data = plistlib.loads(info_path.read_bytes())
            dexts.append({"identifier": data.get("CFBundleIdentifier"),
                          "version": data.get("CFBundleVersion"), "path": str(path),
                          "state": "installed; load state not established"})
    sep = Path("/System/Volumes/iSCPreboot/SFR/current/sep-firmware.img4")
    sep_info = {"path": str(sep), "present": sep.is_file(), "manifest_digest_verified": False}
    if sep.is_file():
        sep_info["sha256"] = file_hash(sep)
    report = {"model": model, "board": board, "product_build": build,
              "loaded_drivers": loaded_drivers(capture("kmutil", "showloaded", "--list-only").decode()),
              "boot_objects": str(boot), "build_manifest": str(manifest_path),
              "build_manifest_sha256": file_hash(manifest_path),
              "active_fud_components": firmware,
              "active_fud_digests_passed": bool(firmware) and all(x["im4p_digest_matches"] for x in firmware),
              "shared_sep": sep_info, "on_demand_firmware_root": str(resource_root),
              "on_demand_firmware_files": resources, "installed_driverkit_extensions": dexts,
              "on_demand_firmware_device_mapping_complete": False,
              "complete_stock_stack_loading_proven": False,
              "custom_boot_firmware_staged": False}
    target = OUT / "driver-check"
    target.mkdir(parents=True, exist_ok=True)
    (target / "stock-inventory.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Stock target: {model}/{board}, {build}; {len(report['loaded_drivers'])} loaded Apple components")
    print(f"Active FUD: {sum(x['im4p_digest_matches'] for x in firmware)}/{len(firmware)} manifest digests match")
    print(f"On-demand firmware: {len(resources)} files inventoried; device mapping incomplete")
    print(f"DriverKit: {len(dexts)} installed extensions inventoried; load state not established")
    if not report["active_fud_digests_passed"]:
        raise SystemExit("active firmware digest gate failed")


if __name__ == "__main__":
    main()
