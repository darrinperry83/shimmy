import os,subprocess,signal,time,resource,sys,shutil
base=os.getcwd();env=os.environ.copy();env.update(RDUID0='1',RDCONS_DUMP='1',RDCONS_DUMPFILE=base+'/build/init-console9.log',RDSOCKETGROUP='/tmp/rd-login-test9',RDEXECDBG='1',RDVT_TRACE='1')
os.makedirs('/tmp/rd-login-test9',exist_ok=True)
probe='tests/login-root/etc/systemd/system/getty@tty1.service.d/probe.conf'
shutil.copyfile('tests/probe-getty.conf',probe)
found=False
with open('build/init-run9.log','w') as out:
 p=subprocess.Popen(['./shim_cli_login_candidate','tests/login-root','/sbin/init','--system','--unit=login-probe.target'],env=env,stdout=out,stderr=out,start_new_session=True,preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
 open('build/init-test9.pid','w').write(str(p.pid))
 try:
  for i in range(120):
   time.sleep(.25)
   if p.poll() is not None:break
   if os.path.exists(env['RDCONS_DUMPFILE']) and 'login:' in open(env['RDCONS_DUMPFILE']).read():
    found=True;print('LOGIN prompt captured',flush=True);time.sleep(3);break
 finally:
  # Services may create new sessions; stop only exact candidate/test-root processes.
  listing=subprocess.check_output(['ps','-axo','pid=,command='],text=True)
  for line in listing.splitlines():
   fields=line.strip().split(None,1)
   if len(fields)==2 and fields[1].startswith('./shim_cli_login_candidate tests/login-root '):
    try:os.kill(int(fields[0]),signal.SIGKILL)
    except ProcessLookupError:pass
  try:p.wait(timeout=3)
  except subprocess.TimeoutExpired:p.kill();p.wait()
 print('init test PID',p.pid,'exit',p.returncode,flush=True)

if os.path.exists(probe):os.unlink(probe)

sys.exit(0 if found else 1)
