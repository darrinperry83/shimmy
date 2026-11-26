#!/usr/bin/env python3
"""Build a service-launch fixture and compare the same ELF/Arch child on Linux."""
import hashlib,json,subprocess,shutil
from pathlib import Path
HERE=Path(__file__).resolve().parent;OUT=HERE.parent/'out/launch'
IMAGE='sha256:0151e70bd01e7e4c891e903edf1dacba81656f04c6150bfce78d5a570e4df443'
def run(argv):
 r=subprocess.run(list(map(str,argv)),text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=90)
 if r.returncode:raise RuntimeError(r.stdout)
 return r.stdout
def main():
 OUT.mkdir(exist_ok=True);root=OUT/'rootfs';(root/'usr/bin').mkdir(parents=True,exist_ok=True);(root/'work').mkdir(exist_ok=True)
 run(['xcrun','clang','--target=aarch64-linux-gnu','-ffreestanding','-fno-builtin','-fno-stack-protector','-ffixed-x18','-O2','-Wall','-Wextra','-Werror','-c',HERE/'probe.c','-o',OUT/'launch-probe.o'])
 run(['ld.lld','-m','aarch64elf','-z','max-page-size=16384','--build-id=none','-static','-e','_start',OUT/'launch-probe.o','-o',root/'usr/bin/launch-probe'])
 run(['xcrun','clang','-O2','-Wall','-Wextra','-Werror',HERE/'run.c','-o',OUT/'launch-run'])
 for relative in ('usr/bin/printf','usr/lib/ld-linux-aarch64.so.1','usr/lib/libc.so.6'):
  dest=root/relative;dest.parent.mkdir(parents=True,exist_ok=True)
  run(['docker','run','--rm','--network','none','-v',f'{OUT}:/capture',IMAGE,'cp','-L','/'+relative,'/capture/rootfs/'+relative])
 if not (root/'lib').exists():(root/'lib').symlink_to('usr/lib')
 output=run(['docker','run','--rm','--network','none','-v',f'{HERE.parent}:/work',IMAGE,'sh','-c','cc -O2 -Wall -Wextra -Werror -Wno-misleading-indentation /work/launch/run.c -o /tmp/launch-run && /tmp/launch-run /work/out/launch/rootfs'])
 if 'XNU_LAUNCH_PERSONALITY_PASS' not in output:raise RuntimeError(output)
 report=dict(linux_image=IMAGE,linux_kernel=run(['docker','run','--rm','--network','none',IMAGE,'uname','-r']).strip(),linux_reference=output,
  files={str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted((root/'usr').rglob('*')) if p.is_file() and not p.is_symlink()},xnu_runtime_pass=False)
 (OUT/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
