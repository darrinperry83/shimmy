#!/usr/bin/env python3
"""Build the unchanged Linux event fixture and run its Linux reference."""
import argparse,hashlib,json,subprocess
from pathlib import Path
HERE=Path(__file__).resolve().parent;OUT=HERE.parent/'out/events'
IMAGE='sha256:0151e70bd01e7e4c891e903edf1dacba81656f04c6150bfce78d5a570e4df443'
def run(argv):
 r=subprocess.run(list(map(str,argv)),text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=90)
 if r.returncode:raise RuntimeError(r.stdout)
 return r.stdout
def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--linux-reference',action='store_true');args=parser.parse_args();OUT.mkdir(exist_ok=True)
 run(['xcrun','clang','--target=aarch64-linux-gnu','-ffreestanding','-fno-builtin','-fno-stack-protector','-ffixed-x18','-O2','-Wall','-Wextra','-Werror','-c',HERE/'probe.c','-o',OUT/'events-probe.o'])
 run(['ld.lld','-m','aarch64elf','-z','max-page-size=16384','--build-id=none','-static','-e','_start',OUT/'events-probe.o','-o',OUT/'events-probe'])
 run(['xcrun','clang','-O2','-Wall','-Wextra','-Werror',HERE/'run.c','-o',OUT/'events-run'])
 report=dict(linux_elf_sha256=hashlib.sha256((OUT/'events-probe').read_bytes()).hexdigest(),xnu_runtime_pass=False)
 if args.linux_reference:
  output=run(['docker','run','--rm','--network','none','-v',f'{HERE.parent}:/work:ro',IMAGE,'sh','-c','cc -O2 -Wall -Wextra -Werror -Wno-misleading-indentation /work/events/run.c -o /tmp/events-run && /tmp/events-run /work/out/events/events-probe'])
  if 'XNU_EVENTS_PERSONALITY_PASS' not in output:raise RuntimeError(output)
  report.update(linux_image=IMAGE,linux_reference=output,linux_kernel=run(['docker','run','--rm','--network','none',IMAGE,'uname','-r']).strip())
 (OUT/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
