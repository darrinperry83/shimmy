#!/usr/bin/env python3
"""Build direct-ELF fixture and pin a minimal unchanged Arch execution root."""
import argparse,hashlib,json,shutil,struct,subprocess
from pathlib import Path
HERE=Path(__file__).resolve().parent
ABI=HERE.parent
OUT=ABI/'out/interp'
IMAGE='sha256:0151e70bd01e7e4c891e903edf1dacba81656f04c6150bfce78d5a570e4df443'
def run(argv,separate=False):
 r=subprocess.run(list(map(str,argv)),text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE if separate else subprocess.STDOUT,timeout=90)
 if r.returncode:raise RuntimeError(r.stdout)
 return r if separate else r.stdout
def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--linux-reference',action='store_true');args=parser.parse_args()
 OUT.mkdir(exist_ok=True);root=OUT/'rootfs';(root/'usr/bin').mkdir(parents=True,exist_ok=True)
 run(['xcrun','clang','--target=aarch64-linux-gnu','-ffreestanding','-fno-builtin','-fno-stack-protector','-ffixed-x18','-ftls-model=local-exec','-fPIE','-O2','-Wall','-Wextra','-Werror','-c',HERE/'probe.c','-o',OUT/'interp-probe.o'])
 run(['ld.lld','-m','aarch64elf','-z','max-page-size=16384','--build-id=none','-pie','--dynamic-linker=/lib/ld-linux-aarch64.so.1','-e','_start',OUT/'interp-probe.o','-o',root/'usr/bin/interp-probe'])
 run(['xcrun','clang','-O2','-Wall','-Wextra','-Werror',HERE/'run.c','-o',OUT/'interp-run'])
 reference=json.loads((ABI/'out/systemd/reference.json').read_text())
 for relative,digest in reference['files'].items():
  source=ABI/'out/systemd/rootfs'/relative
  if hashlib.sha256(source.read_bytes()).hexdigest()!=digest:raise ValueError(relative)
  target=root/relative;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,target)
 shutil.copy2(ABI/'out/process/arch-printf',root/'usr/bin/printf')
 for alias,target in [('lib','usr/lib'),('bin','usr/bin')]:
  p=root/alias
  if not p.is_symlink():p.symlink_to(target)
 program=(root/'usr/bin/interp-probe').read_bytes();phoff=struct.unpack_from('<Q',program,32)[0];phnum=struct.unpack_from('<H',program,56)[0]
 offset=next(struct.unpack_from('<Q',program,phoff+i*56+8)[0] for i in range(phnum) if struct.unpack_from('<I',program,phoff+i*56)[0]==3)
 for label,folder in [('missing','bad'),('denied','den'),('broken','err')]:
  changed=bytearray(program);changed[offset+1:offset+4]=folder.encode();(root/f'usr/bin/{label}-probe').write_bytes(changed)
  if label!='missing':
   p=root/folder/'ld-linux-aarch64.so.1';p.parent.mkdir(exist_ok=True)
   if label=='denied':shutil.copy2(root/'usr/lib/ld-linux-aarch64.so.1',p);p.chmod(0o644)
   else:p.write_bytes(b'This is not an ELF interpreter.\n');p.chmod(0o755)
 for p in (root/'usr/bin').iterdir():p.chmod(0o755)
 run(['xcrun','clang','-O1','-g','-fsanitize=address,undefined','-Wall','-Wextra','-Werror',HERE/'test_plan.c','-o',OUT/'test-plan'])
 plan=run([OUT/'test-plan',root/'usr/bin/interp-probe',root/'usr/lib/ld-linux-aarch64.so.1'])
 report=dict(plan_check=plan,files={str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(root.rglob('*')) if p.is_file() and not p.is_symlink()},linux_reference_pass=False,xnu_runtime_pass=False)
 if args.linux_reference:
  captured=run(['docker','run','--rm','--network','none','-v',f'{ABI}:/work',IMAGE,'sh','-c','cc -O2 -Wall -Wextra -Werror /work/interp/run.c -o /tmp/interp-run && /tmp/interp-run /work/out/interp/rootfs'],separate=True)
  result=captured.stdout
  (OUT/'reference-stdout.log').write_text(captured.stdout);(OUT/'reference-stderr.log').write_text(captured.stderr)
  for marker in ('ARCH_DIRECT_CHILD_PASS','LINUX_INTERP_PASS','INTERP_ERROR_CONTROLS_PASS','XNU_INTERP_PERSONALITY_PASS',reference['version_output']):
   if marker not in result:raise RuntimeError(f'missing Linux marker: {marker}\n{result}')
  report.update(linux_reference_pass=True,linux_image=IMAGE,linux_reference=result,linux_stderr=captured.stderr)
 (OUT/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
