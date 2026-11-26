#!/usr/bin/env python3
"""Build and compare the private file-mapping ELF on real AArch64 Linux."""
import argparse
import hashlib
import json
from pathlib import Path
from verify import run
HERE=Path(__file__).resolve().parent
OUT=HERE.parent/'out/memory'


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--linux-reference',action='store_true')
    args=parser.parse_args();OUT.mkdir(exist_ok=True)
    run(['xcrun','clang','--target=aarch64-linux-gnu','-c',HERE/'file_probe.S','-o',OUT/'filemap-probe.o'])
    run(['ld.lld','-m','aarch64elf','-z','max-page-size=16384','--build-id=none','-static','-e','_start',OUT/'filemap-probe.o','-o',OUT/'filemap-probe'])
    run(['xcrun','clang','-O2','-Wall','-Wextra','-Werror',HERE/'file_run.c','-o',OUT/'filemap-run'])
    report={'linux_elf_sha256':hashlib.sha256((OUT/'filemap-probe').read_bytes()).hexdigest(),'xnu_runtime_proven':False}
    if args.linux_reference:
        image=run(['docker','image','inspect','menci/archlinuxarm:base-devel','--format','{{.Id}}']).strip()
        output=run(['docker','run','--rm','--network','none','-v',f'{HERE.parent}:/work:ro',image,'sh','-c',
                    'cc -O2 -Wall -Wextra -Werror /work/memory/file_run.c -o /tmp/filemap-run && /tmp/filemap-run /work/out/memory/filemap-probe'])
        if 'XNU_FILEMAP_PERSONALITY_PASS' not in output:raise RuntimeError('missing reference marker')
        report.update(linux_image=image,linux_reference=output)
    (OUT/'filemap-verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))


if __name__=='__main__':main()
