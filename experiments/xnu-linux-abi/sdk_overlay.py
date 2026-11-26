#!/usr/bin/env python3
"""Make a disposable public-SDK overlay; never writes into Xcode's SDK."""
import argparse
import json
from pathlib import Path
import subprocess

AVAILABILITY_COMMIT = "149b1777f3e8c2133042d8e188f72e3adf6a7110"


def overlay(source, target, branches):
    target.mkdir()
    if source.is_dir():
        for item in source.iterdir():
            if item.name not in branches:
                (target / item.name).symlink_to(item)
    for name, children in branches.items():
        overlay(source / name, target / name, children)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sdk", type=Path)
    parser.add_argument("availability_source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    sdk, source, destination = (p.resolve() for p in
                                (args.sdk, args.availability_source, args.destination))
    if not (sdk / "SDKSettings.json").is_file():
        raise ValueError("expected a macOS SDK")
    commit = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    if commit != AVAILABILITY_COMMIT:
        raise ValueError("AvailabilityVersions checkout does not match recorded pin")
    for name in ("availability", "availability.pl", "availability.dsl"):
        if not (source / name).exists():
            raise ValueError(f"missing AvailabilityVersions input: {name}")
    if destination.exists() or destination.is_symlink():
        raise ValueError("destination already exists; refusing to overwrite")
    overlay(sdk, destination, {"usr": {"local": {"libexec": {}}}})
    for name in ("availability", "availability.pl", "availability.dsl"):
        target = destination / "usr/local/libexec" / name
        if target.is_symlink():
            target.unlink()  # only a link just created inside our disposable overlay
        target.symlink_to(source / name)
    (destination / "rdxnu-overlay.json").write_text(json.dumps({
        "base_sdk": str(sdk), "availability_commit": commit,
        "note": "public SDK plus open-source availability tool, not a complete kernel SDK"
    }, indent=2) + "\n")
    print(destination)


if __name__ == "__main__":
    main()
