#!/usr/bin/env python3
"""Check and link the full stock driver set offline. Never installs or boots."""
import json
import subprocess
import sys
from driver_gate import HERE, OUT


def main():
    target = OUT / "driver-check"
    target.mkdir(exist_ok=True)
    result = {"checks": {}, "runtime_driver_support_proven": False}
    for name, script in (("stock_inventory", "stock_inventory.py"),
                         ("stock_boot_collection", "stock_collection.py"),
                         ("stock_inventory_controls", "test_stock_inventory.py"),
                         ("kernel_artifact", "audit_kernel.py"),
                         ("symbol_regressions", "driver_gate.py"),
                         ("vtables", "vtable_gate.py"),
                         ("sep_interfaces", "audit_sep.py"),
                         ("negative_controls", "test_driver_gates.py")):
        code = subprocess.run([sys.executable, str(HERE / script)]).returncode
        if name == "symbol_regressions" and code in (0, 1):
            # Keep raw uncertainties visible; a complete native link must
            # resolve them. Never waive additional missing kernel interfaces.
            symbols = json.loads((target / "symbol-report.json").read_text())
            result["raw_symbol_scan_passed"] = symbols["static_symbol_gate_passed"]
            result["checks"][name] = all(not p["missing_dependencies"] and
                not p["inspection_errors"] and p["newly_unresolved_import_count"] == 0
                for p in symbols["profiles"].values())
        else:
            result["checks"][name] = code == 0
    if all(result["checks"].values()):
        code = subprocess.run([sys.executable, str(HERE / "link_stock_collection.py")]).returncode
        result["checks"]["collection_link_and_contents"] = code == 0
        if not code:
            result["collection"] = json.loads((target / "full-stock-link-result.json").read_text())
    else:
        result["checks"]["collection_link_and_contents"] = False
    result["driver_preboot_gate_passed"] = all(result["checks"].values())
    (target / "preboot-report.json").write_text(json.dumps(result, indent=2) + "\n")
    print("DRIVER_PREBOOT_GATE_PASS" if result["driver_preboot_gate_passed"] else
          "DRIVER_PREBOOT_GATE_BLOCKED: do not treat this as a boot-ready collection")
    sys.exit(0 if result["driver_preboot_gate_passed"] else 1)


if __name__ == "__main__":
    main()
