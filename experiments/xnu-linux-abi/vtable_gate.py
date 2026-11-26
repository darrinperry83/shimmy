#!/usr/bin/env python3
"""Compare selected IOKit vtable slots to the matching Apple kernel, offline."""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import sys
from driver_gate import OUT, digest

CLASSES = {"OSObject": "__ZTV8OSObject", "IOService": "__ZTV9IOService",
           "IOUserClient": "__ZTV12IOUserClient", "IOMemoryDescriptor": "__ZTV18IOMemoryDescriptor",
           "IOWorkLoop": "__ZTV10IOWorkLoop", "IOCommandGate": "__ZTV13IOCommandGate",
           "IOMemoryMap": "__ZTV11IOMemoryMap", "IODMACommand": "__ZTV12IODMACommand",
           "IOBufferMemoryDescriptor": "__ZTV24IOBufferMemoryDescriptor",
           "IOGeneralMemoryDescriptor": "__ZTV25IOGeneralMemoryDescriptor",
           "IOInterruptEventSource": "__ZTV22IOInterruptEventSource",
           "IOTimerEventSource": "__ZTV18IOTimerEventSource",
           "SEPHibernator": "__ZTV13SEPHibernator",
           "SEPHibernatorService": "__ZTV20SEPHibernatorService",
           "SEPHibernator::MetaClass": "__ZTVN13SEPHibernator9MetaClassE",
           "SEPHibernatorService::MetaClass": "__ZTVN20SEPHibernatorService9MetaClassE"}


class Image:
    def __init__(self, path):
        self.path = path
        self.data = path.read_bytes()
        if struct.unpack_from("<I", self.data)[0] != 0xfeedfacf:
            raise ValueError("expected thin 64-bit little-endian Mach-O")
        self.sections = []
        segments = []
        offset = 32
        for _ in range(struct.unpack_from("<I", self.data, 16)[0]):
            cmd, length = struct.unpack_from("<II", self.data, offset)
            if length < 8 or offset + length > len(self.data):
                raise ValueError("malformed load command")
            if cmd == 0x19:  # LC_SEGMENT_64
                if self.data[offset + 8:offset + 24].rstrip(b"\0") == b"__TEXT":
                    segments.append(struct.unpack_from("<Q", self.data, offset + 24)[0])
                for i in range(struct.unpack_from("<I", self.data, offset + 64)[0]):
                    pos = offset + 72 + 80 * i
                    address, size, file_offset = struct.unpack_from("<QQI", self.data, pos + 32)
                    self.sections.append((address, size, file_offset))
            offset += length
        self.base = min(segments)
        self.names, self.addresses = {}, {}
        output = subprocess.check_output(["xcrun", "llvm-nm", "--arch=arm64e", "-n",
                                          "--defined-only", str(path)], text=True)
        for line in output.splitlines():
            parts = line.split()
            if len(parts) == 3:
                try:
                    address = int(parts[0], 16)
                except ValueError:
                    continue
                self.names[parts[2]] = address
                self.addresses.setdefault(address, set()).add(parts[2])

    def table(self, symbol):
        start = self.names[symbol]
        section, size, file_offset = next(s for s in self.sections if s[0] <= start < s[0] + s[1])
        end = min(a for a in self.addresses if start < a <= section + size)
        if (end - start) % 8 or end - start > 16384:
            raise ValueError("unexpected vtable extent")
        offset = file_offset + start - section
        count = (end - start) // 8
        values = struct.unpack_from(f"<{count}Q", self.data, offset)
        if values[:2] != (0, 0):
            raise ValueError("expected a primary Itanium vtable without RTTI")
        result = []
        for value in values[2:]:
            if value == 0:
                result.append({"symbols": ["NULL"], "authentication": None})
                continue
            if value in self.addresses:
                address, auth = value, None
            elif value >> 63 == 1 and (value >> 62) & 1 == 0:
                # dyld_chained_ptr_arm64e_auth_rebase, kernel VM-relative target.
                # See the SDK's mach-o/fixup-chains.h. Ignore only chain 'next'.
                address = self.base + (value & 0xffffffff)
                auth = {"diversity": (value >> 32) & 0xffff,
                        "address_diversity": (value >> 48) & 1,
                        "key": (value >> 49) & 3}
            else:
                raise ValueError(f"unsupported vtable pointer encoding {value:#x}")
            if address not in self.addresses:
                raise ValueError(f"vtable target {address:#x} has no exact symbol")
            result.append({"symbols": sorted(self.addresses[address]), "authentication": auth})
        # The next data symbol may follow zero alignment padding. It is not a
        # virtual method. Keep internal NULLs, but remove padding at the end.
        while result and result[-1]["symbols"] == ["NULL"]:
            result.pop()
        return result


def compare(a, b):
    mismatches = []
    for slot in range(max(len(a), len(b))):
        left = a[slot] if slot < len(a) else None
        right = b[slot] if slot < len(b) else None
        if (left is None or right is None or
            not set(left["symbols"]).intersection(right["symbols"]) or
            left["authentication"] != right["authentication"]):
            mismatches.append({"method_slot": slot, "custom": left, "control": right})
    return mismatches


def audit(machine="T6020"):
    if machine not in ("T6020", "VMAPPLE"):
        raise ValueError("unsupported machine")
    dependency = json.loads((OUT / "prepared-dependencies.json").read_text())
    custom = Image(OUT / f"xnu/BUILD/obj/DEVELOPMENT_ARM64_{machine}/kernel.development.{machine.lower()}.unstripped")
    stock = Image(Path(dependency["kdk_root"]) / f"System/Library/Kernels/kernel.development.{machine.lower()}")
    report = {"kernel_sha256": digest(custom.path), "control_sha256": digest(stock.path),
              "machine": machine,
              "scope": "selected primary IOKit vtables, slot names and pointer authentication",
              "runtime_driver_support_proven": False, "classes": {}}
    for name, symbol in CLASSES.items():
        # SEP is part of the physical target's gate. A VM does not exercise SEP.
        if machine == "VMAPPLE" and name.startswith("SEP"):
            continue
        a, b = custom.table(symbol), stock.table(symbol)
        mismatches = compare(a, b)
        report["classes"][name] = {"custom_slot_count": len(a), "control_slot_count": len(b),
                                  "mismatches": mismatches, "passed": not mismatches}
        print(f"{name}: custom={len(a)} Apple={len(b)} slots; {len(mismatches)} mismatches")
    report["vtable_gate_passed"] = all(x["passed"] for x in report["classes"].values())
    target = OUT / ("driver-check" if machine == "T6020" else "vmapple") / "vtable-report.json"
    target.write_text(json.dumps(report, indent=2) + "\n")
    print(f"Report: {target}")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--machine", choices=("T6020", "VMAPPLE"), default="T6020")
    args = parser.parse_args()
    sys.exit(0 if audit(args.machine)["vtable_gate_passed"] else 1)


if __name__ == "__main__":
    main()
