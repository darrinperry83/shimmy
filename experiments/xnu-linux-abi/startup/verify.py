#!/usr/bin/env python3
"""Build the unchanged Linux startup fixture and its native parent."""
import argparse,hashlib,json,subprocess
from pathlib import Path
HERE=Path(__file__).resolve().parent
OUT=HERE.parent/'out/startup'
def run(argv):
    result=subprocess.run(list(map(str,argv)),text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=90)
    if result.returncode:raise RuntimeError(result.stdout)
    return result.stdout
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--linux-reference',action='store_true')
    args=parser.parse_args();OUT.mkdir(exist_ok=True)
    run(['xcrun','clang','--target=aarch64-linux-gnu','-ffreestanding','-fno-builtin','-fno-stack-protector','-ffixed-x18','-O2','-Wall','-Wextra','-Werror','-c',HERE/'probe.c','-o',OUT/'startup-probe.o'])
    run(['ld.lld','-m','aarch64elf','-z','max-page-size=16384','--build-id=none','-static','-e','_start',OUT/'startup-probe.o','-o',OUT/'startup-probe'])
    run(['xcrun','clang','-O2','-Wall','-Wextra','-Werror',HERE/'run.c','-o',OUT/'startup-run'])
    report={'linux_elf_sha256':hashlib.sha256((OUT/'startup-probe').read_bytes()).hexdigest(),'xnu_runtime_proven':False}
    if args.linux_reference:
        image=run(['docker','image','inspect','menci/archlinuxarm:base-devel','--format','{{.Id}}']).strip()
        output=run(['docker','run','--rm','--network','none','-v',f'{HERE.parent}:/work:ro',image,'sh','-c','cc -O2 -Wall -Wextra -Werror /work/startup/run.c -o /tmp/startup-run && /tmp/startup-run /work/out/startup/startup-probe'])
        if 'XNU_STARTUP_PERSONALITY_PASS' not in output:raise RuntimeError('reference marker missing')
        report.update(linux_image=image,linux_reference=output)
    (OUT/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
