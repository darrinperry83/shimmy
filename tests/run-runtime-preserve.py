#!/usr/bin/env python3
"""A standalone guest command must preserve an existing runtime directory."""
import os
from pathlib import Path
import resource
import subprocess
import tempfile

repo = Path(__file__).resolve().parent.parent
source = '''void _start(void) {
register long x0 __asm__("x0")=0,x8 __asm__("x8")=93;
__asm__ volatile("svc #0"::"r"(x0),"r"(x8):"memory");for(;;){}
}'''

def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

with tempfile.TemporaryDirectory(prefix="runtime-preserve-", dir=repo / "build") as temp:
    root = Path(temp)
    marker = root / "run/user/963/sentinel"
    marker.parent.mkdir(parents=True)
    marker.write_text("running session\n")
    c = root / "probe.c"
    c.write_text(source)
    subprocess.run(["clang", "--target=aarch64-linux-gnu", "-nostdlib", "-static-pie",
                    "-fuse-ld=lld", "-Wl,-e,_start", str(c), "-o", str(root / "probe")], check=True)
    env = os.environ.copy()
    for key in ("RDVT_ROOT_PID", "RDSKIP_RUNCLEAR", "RDSHIM_BOOT", "RDBOOT_LOCK_FD"):
        env.pop(key, None)
    result = subprocess.run([os.environ.get("RD_TEST_BINARY", str(repo / "shim_cli")),
                             str(root), "/probe"], env=env, capture_output=True,
                            text=True, timeout=10, preexec_fn=no_core)
    if result.returncode or not marker.exists() or marker.read_text() != "running session\n":
        raise SystemExit("RUNTIME_PRESERVE_FAILED: standalone command changed /run")
    print("RUNTIME_PRESERVE_OK")
