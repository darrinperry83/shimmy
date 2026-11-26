#!/usr/bin/env python3
"""Build the Linux memory fixture and native parent; compare on real Linux."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
HERE=Path(__file__).resolve().parent
OUT=HERE.parent/'out/memory'


def run(command):
    result=subprocess.run([str(x) for x in command],capture_output=True,text=True,timeout=90)
    if result.returncode:
        raise RuntimeError(f'{command!r}:\n{result.stdout}{result.stderr}')
    return result.stdout


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--linux-reference',action='store_true')
    args=parser.parse_args();OUT.mkdir(exist_ok=True)
    run(['xcrun','clang','--target=aarch64-linux-gnu','-c',HERE/'probe.S','-o',OUT/'memory-probe.o'])
    run(['ld.lld','-m','aarch64elf','-z','max-page-size=16384','--build-id=none','-static','-e','_start',OUT/'memory-probe.o','-o',OUT/'memory-probe'])
    run(['xcrun','clang','-O2','-Wall','-Wextra','-Werror',HERE/'run.c','-o',OUT/'memory-run'])
    report={'linux_elf_sha256':hashlib.sha256((OUT/'memory-probe').read_bytes()).hexdigest(),'xnu_runtime_proven':False}
    if args.linux_reference:
        image=run(['docker','image','inspect','menci/archlinuxarm:base-devel','--format','{{.Id}}']).strip()
        output=run(['docker','run','--rm','--network','none','-v',f'{HERE.parent}:/work:ro',image,'sh','-c',
                    'cc -O2 -Wall -Wextra -Werror /work/memory/run.c -o /tmp/memory-run && /tmp/memory-run /work/out/memory/memory-probe'])
        if 'XNU_MEMORY_PERSONALITY_PASS' not in output:raise RuntimeError('missing reference marker')
        report.update(linux_image=image,linux_reference=output)
    (OUT/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))


if __name__=='__main__':main()
