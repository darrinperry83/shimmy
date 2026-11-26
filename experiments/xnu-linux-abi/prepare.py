#!/usr/bin/env python3
"""Prepare a source patch in an explicit XNU checkout; never installs a kernel."""
import argparse
import difflib
import hashlib
from pathlib import Path

COMMIT = "ac9718fb1af618d5ce8678d0dc6e8a58f252216f"
TAG = "xnu-12377.121.6"
SLEH = Path("osfmk/arm64/sleh.c")
SHA256 = "4eeb208fb2510bfdc0338d1a1d7d2d702fe9eb65d332eea388720ef6574375d1"
HERE = Path(__file__).resolve().parent


def patched(source):
    if hashlib.sha256(source).hexdigest() != SHA256:
        raise ValueError(f"sleh source must match {TAG} ({COMMIT}); refusing other content")
    text = source.decode()
    anchor = "#include <sys/errno.h>\n"
    assert text.count(anchor) == 1
    text = text.replace(anchor, anchor + "#include <pexpert/pexpert.h>\n"
                        '#include "rdxnu_linux_probe.h"\n', 1)
    anchor = "\tcurrent_cached_proc_cred_update();\n\n\tif (trap_no < 0) {"
    assert text.count(anchor) == 1
    text = text.replace(anchor, "\tcurrent_cached_proc_cred_update();\n\n"
        "\t/* RDXNU P0: experimental, boot-gated Linux syscall entry. */\n"
        "\tp = get_bsdthreadtask_info(thread);\n"
        "\tif (p && rdxnu_linux_probe(state, p)) {\n"
        "\t\tthread->syscalls_unix++;\n"
        "\t\treturn;\n"
        "\t}\n\n\tif (trap_no < 0) {", 1)
    return text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("xnu_root", type=Path)
    parser.add_argument("--apply", action="store_true", help="edit the explicit source checkout")
    args = parser.parse_args()
    target = args.xnu_root / SLEH
    original = target.read_bytes()
    result = patched(original)
    print("".join(difflib.unified_diff(original.decode().splitlines(True),
          result.splitlines(True), fromfile=f"a/{SLEH}", tofile=f"b/{SLEH}")), end="")
    if args.apply:
        header = target.with_name("rdxnu_linux_probe.h")
        if header.exists():
            raise ValueError(f"refusing to overwrite {header}")
        header.write_bytes((HERE / header.name).read_bytes())
        target.write_text(result)
        print(f"Prepared {args.xnu_root}. No kernel built or installed.")


if __name__ == "__main__":
    main()
