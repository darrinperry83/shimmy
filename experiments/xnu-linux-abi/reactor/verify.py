#!/usr/bin/env python3
"""Build the event-loop batch fixture and run the unchanged ELF on Linux."""
import argparse,hashlib,json,subprocess,shutil,re
from pathlib import Path
HERE=Path(__file__).resolve().parent;OUT=HERE.parent/'out/reactor'
IMAGE='sha256:0151e70bd01e7e4c891e903edf1dacba81656f04c6150bfce78d5a570e4df443'
def run(argv):
 r=subprocess.run(list(map(str,argv)),text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=100)
 if r.returncode:raise RuntimeError(r.stdout)
 return r.stdout
def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--linux-reference',action='store_true');args=parser.parse_args();OUT.mkdir(exist_ok=True)
 run(['xcrun','clang','--target=aarch64-linux-gnu','-ffreestanding','-fno-builtin','-fno-stack-protector','-ffixed-x18','-O2','-Wall','-Wextra','-Werror','-c',HERE/'probe.c','-o',OUT/'reactor-probe.o'])
 run(['ld.lld','-m','aarch64elf','-z','max-page-size=16384','--build-id=none','-static','-e','_start',OUT/'reactor-probe.o','-o',OUT/'reactor-probe'])
 run(['xcrun','clang','-O2','-Wall','-Wextra','-Werror',HERE/'run.c','-o',OUT/'reactor-run'])
 run(['xcrun','clang','-O2','-Wall','-Wextra','-Werror',HERE/'sd_event_run.c','-o',OUT/'sd-event-run'])
 report=dict(linux_elf_sha256=hashlib.sha256((OUT/'reactor-probe').read_bytes()).hexdigest(),xnu_runtime_pass=False)
 if args.linux_reference:
  output=run(['docker','run','--rm','--network','none','-v',f'{HERE.parent}:/work:ro',IMAGE,'sh','-c','cc -O2 -Wall -Wextra -Werror -Wno-misleading-indentation /work/reactor/run.c -o /tmp/reactor-run && /tmp/reactor-run /work/out/reactor/reactor-probe'])
  if 'XNU_REACTOR_PERSONALITY_PASS' not in output:raise RuntimeError(output)
  root=OUT/'rootfs';root.mkdir(exist_ok=True)
  build=run(['docker','run','--rm','--network','none','-v',f'{HERE.parent}:/work',IMAGE,'sh','-c','cc -O2 -Wall -Wextra -Werror -Wno-misleading-indentation /work/reactor/sd_event.c -lsystemd -o /work/out/reactor/sd-event-probe && ldd /work/out/reactor/sd-event-probe'])
  (OUT/'sd-event-dependencies.txt').write_text(build)
  files=sorted(set(re.findall(r'(/[^\s()]+)',build)))
  if not files:raise RuntimeError(build)
  for relative in files:
   target=root/relative.lstrip('/');target.parent.mkdir(parents=True,exist_ok=True)
   run(['docker','run','--rm','--network','none','-v',f'{OUT}:/capture',IMAGE,'cp','-L',relative,'/capture/rootfs/'+relative.lstrip('/')])
  (root/'usr/bin').mkdir(parents=True,exist_ok=True);shutil.copy2(OUT/'sd-event-probe',root/'usr/bin/sd-event-probe')
  if not (root/'lib').is_symlink():
   # ldd may name /lib/ld-linux; normalize the original Arch /lib alias.
   lib=root/'lib'
   if lib.exists():
    for file in lib.iterdir():
     dest=root/'usr/lib'/file.name;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(file,dest)
    shutil.rmtree(lib)
   lib.symlink_to('usr/lib')
  original=run(['docker','run','--rm','--network','none','-v',f'{OUT}:/capture:ro',IMAGE,'/capture/sd-event-probe'])
  copied=run(['docker','run','--rm','--network','none','-v',f'{OUT}:/capture:ro',IMAGE,'chroot','/capture/rootfs','/usr/bin/sd-event-probe'])
  if original!=copied or not original.startswith('ARCH_SD_EVENT_LOOP_PASS:'):raise RuntimeError(original+copied)
  report.update(linux_image=IMAGE,linux_reference=output,linux_kernel=run(['docker','run','--rm','--network','none',IMAGE,'uname','-r']).strip(),sd_event_reference=original,files={str(file.relative_to(root)):hashlib.sha256(file.read_bytes()).hexdigest() for file in sorted(root.rglob('*')) if file.is_file() and not file.is_symlink()})
 (OUT/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
