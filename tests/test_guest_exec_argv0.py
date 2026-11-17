#!/usr/bin/env python3
"""Check that a guest execve preserves the caller's argv[0] across shim re-exec."""

import argparse
import os
import re
import resource
import subprocess
from pathlib import Path


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True,
                        help="disposable Linux guest root")
    parser.add_argument("--cli", type=Path, default=Path("./shim_cli"))
    args = parser.parse_args()
    root = args.root.resolve(strict=True)
    cli = args.cli.resolve(strict=True)
    command = ('exec -a -bash /bin/bash -c '
               "'shopt login_shell; printf \"ARGV0=%s\\n\" \"$0\"'")
    result = subprocess.run([str(cli), str(root), "/bin/bash", "-c", command],
                            env={**os.environ, "RDUID0": "1"},
                            capture_output=True, text=True, timeout=30,
                            preexec_fn=no_core)
    if result.returncode or not re.search(r"(?m)^login_shell\s+on$", result.stdout) or \
            "ARGV0=-bash" not in result.stdout:
        raise SystemExit(f"guest argv0 lost across exec: rc={result.returncode}\n"
                         f"stdout={result.stdout}\nstderr={result.stderr[-1200:]}")
    print("guest execve preserved -bash; Bash entered login-shell mode")


if __name__ == "__main__":
    main()
