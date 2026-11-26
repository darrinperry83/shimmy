#!/usr/bin/env python3
"""Build the unchanged Linux process-lifecycle fixture and its native parent."""
import argparse,hashlib,json,subprocess
from pathlib import Path
HERE=Path(__file__).resolve().parent
OUT=HERE.parent/'out/process'
def run(argv):
    result=subprocess.run(list(map(str,argv)),text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=90)
    if result.returncode:raise RuntimeError(result.stdout)
    return result.stdout
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--linux-reference',action='store_true')
    args=parser.parse_args();OUT.mkdir(exist_ok=True)
    run(['xcrun','clang','--target=aarch64-linux-gnu','-ffreestanding','-fno-builtin','-fno-stack-protector','-ffixed-x18','-O2','-Wall','-Wextra','-Werror','-c',HERE/'probe.c','-o',OUT/'process-probe.o'])
    run(['ld.lld','-m','aarch64elf','-z','max-page-size=16384','--build-id=none','-static','-e','_start',OUT/'process-probe.o','-o',OUT/'process-probe'])
    run(['xcrun','clang','-O2','-Wall','-Wextra','-Werror',HERE/'run.c','-o',OUT/'process-run'])
    report={'linux_elf_sha256':hashlib.sha256((OUT/'process-probe').read_bytes()).hexdigest(),'xnu_runtime_proven':False}
    if args.linux_reference:
        image=run(['docker','image','inspect','menci/archlinuxarm:base-devel','--format','{{.Id}}']).strip()
        run(['docker','run','--rm','--network','none','-v',f'{OUT}:/capture',image,'cp','/usr/bin/printf','/capture/arch-printf'])
        report['arch_printf_sha256']=hashlib.sha256((OUT/'arch-printf').read_bytes()).hexdigest()
        output=run(['docker','run','--rm','--network','none','-v',f'{HERE.parent}:/work:ro',image,'sh','-c','cc -O2 -Wall -Wextra -Werror /work/process/run.c -o /tmp/process-run && /tmp/process-run /work/out/process/process-probe /usr/lib/ld-linux-aarch64.so.1 /usr/lib /work/out/process/arch-printf'])
        if 'XNU_PROCESS_PERSONALITY_PASS' not in output:raise RuntimeError('reference marker missing')
        report.update(linux_image=image,linux_reference=output)
    (OUT/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
