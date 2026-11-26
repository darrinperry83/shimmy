#!/usr/bin/env python3
"""Compare reconstructed SEP base interfaces with Apple's matching KDK evidence."""
import json
from pathlib import Path
import re
import subprocess
from driver_gate import HERE, OUT, inventory, symbols, digest
from vtable_gate import Image, compare

GETTER = "__ZN13SEPHibernator13sepHibernatorEv"
START = "__ZN20SEPHibernatorService5startEP9IOService"


def capture(*args):
    return subprocess.check_output(list(map(str, args)), text=True)


def function(path, name):
    text = capture("xcrun", "llvm-objdump", "-d", f"--disassemble-symbols={name}", path)
    if f"<{name}>:" not in text:
        raise ValueError(f"missing function {name}")
    return text


def matching_name(image, disassembly):
    registers = {}
    for line in disassembly.splitlines():
        adrp = re.search(r"\badrp\s+(x\d+),\s*(0x[0-9a-f]+)", line)
        add = re.search(r"\badd\s+(x\d+),\s*(x\d+),\s*#(0x[0-9a-f]+|\d+)", line)
        if adrp:
            registers[adrp[1]] = int(adrp[2], 16)
        elif add and add[2] in registers:
            registers[add[1]] = registers[add[2]] + int(add[3], 0)
        if "<_" in line and "serviceMatchingEPKc" in line:
            address = registers["x0"]
            for start, size, offset in image.sections:
                if start <= address < start + size:
                    position = offset + address - start
                    return image.data[position:image.data.index(0, position)].decode()
    raise ValueError("cannot establish matching-service argument from generated code")


def words(disassembly):
    return re.findall(r"^[0-9a-f]+:\s+([0-9a-f]{8})\s", disassembly, re.M)


def layout(dsym, name, member):
    text = capture("xcrun", "llvm-dwarfdump", f"--name={name}", dsym)
    for block in text.split("\n\n"):
        if "DW_TAG_class_type" not in block or f'("{name}")' not in block:
            continue
        size = re.search(r"DW_AT_byte_size\s+\((0x[0-9a-f]+)\)", block)
        if not size:
            continue
        offset = re.search(r"(0x[0-9a-f]+): DW_TAG_class_type", block)[1]
        children = capture("xcrun", "llvm-dwarfdump", f"--debug-info={offset}", "--show-children", dsym)
        for child in children.split("\n\n"):
            if "DW_TAG_member" in child and f'("{member}")' in child:
                location = re.search(r"DW_AT_data_member_location\s+\((0x[0-9a-f]+)\)", child)
                if location:
                    return {"size": int(size[1], 16), "member_offset": int(location[1], 16)}
    raise ValueError(f"cannot establish {name} object layout from DWARF")


def audit():
    kdk = Path(json.loads((OUT / "prepared-dependencies.json").read_text())["kdk_root"])
    custom_path = OUT / "xnu/BUILD/obj/DEVELOPMENT_ARM64_T6020/kernel.development.t6020.unstripped"
    stock_path = kdk / "System/Library/Kernels/kernel.development.t6020"
    custom, stock = Image(custom_path), Image(stock_path)
    report = {"kernel_sha256": digest(custom_path), "control_sha256": digest(stock_path),
              "runtime_sep_operation_proven": False, "full_hibernation_proven": False,
              "checks": {}, "layouts": {}}
    for name, symbol in (("SEPHibernator", "__ZTV13SEPHibernator"),
                         ("SEPHibernatorService", "__ZTV20SEPHibernatorService"),
                         ("SEPHibernatorMetaClass", "__ZTVN13SEPHibernator9MetaClassE"),
                         ("SEPHibernatorServiceMetaClass", "__ZTVN20SEPHibernatorService9MetaClassE")):
        report["checks"][name + "_vtable"] = not compare(custom.table(symbol), stock.table(symbol))
    report["checks"]["operation_remains_abstract"] = "___cxa_pure_virtual" in custom.table("__ZTV13SEPHibernator")[39]["symbols"]
    for name, field, expected in (("SEPHibernator", "reserved", {"size": 0x18, "member_offset": 0x10}),
                                  ("SEPHibernatorService", "hibernator", {"size": 0x90, "member_offset": 0x88})):
        candidate = layout(custom_path.with_name("kernel.development.t6020.dSYM"), name, field)
        reference = layout(kdk / "System/Library/Kernels/kernel.release.t6020.dSYM", name, field)
        report["layouts"][name] = {"custom": candidate, "reference": reference}
        report["checks"][name + "_layout"] = candidate == reference == expected
    getters = {}
    for label, image, path in (("custom", custom, custom_path), ("stock", stock, stock_path)):
        getter = function(path, GETTER)
        getters[label] = getter
        report["checks"][label + "_matches_concrete_service"] = matching_name(image, getter) == "HibernationService"
        report["checks"][label + "_wait_and_member"] = (
            "waitForMatchingServiceWithToken" in getter and "#-0x1" in getter and "#0x88]" in getter)
    report["checks"]["base_start_machine_code_matches"] = words(function(custom_path, START)) == words(function(stock_path, START))
    bundles = inventory(kdk / "System/Library/Extensions")
    imports = symbols(Path(bundles["com.apple.driver.SEPHibernation"]["executable"]), False)
    required = imports & set((HERE / "kernel/sep.exports").read_text().splitlines())
    report["restored_driver_imports"] = sorted(required)
    report["checks"]["sixteen_driver_imports_defined"] = len(required) == 16 and required <= symbols(custom_path, True)
    report["passed"] = all(report["checks"].values())
    target = OUT / "driver-check"
    (target / "sep-interface-audit.json").write_text(json.dumps(report, indent=2) + "\n")
    (target / "sep-getters-disassembly.txt").write_text(getters["stock"] + "\n" + getters["custom"])
    print("SEP_INTERFACE_AUDIT_PASS" if report["passed"] else "SEP_INTERFACE_AUDIT_FAIL")
    print(json.dumps(report["checks"], indent=2))
    if not report["passed"]:
        raise ValueError("SEP interface evidence does not match the Apple control")
    return report


if __name__ == "__main__":
    audit()
