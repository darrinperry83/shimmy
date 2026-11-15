import os,subprocess,signal,time,resource,sys,shutil
base=os.getcwd();root=os.environ.get('RD_LOGIN_ROOT','tests/login-root');binary=os.environ.get('RD_LOGIN_BINARY','./shim_cli')
env=os.environ.copy();env.update(RDUID0='1',RDCONS_DUMP='1',RDCONS_DUMPFILE=base+'/build/init-console6.log',RDSOCKETGROUP='/tmp/rd-login-test6')
os.makedirs('/tmp/rd-login-test6',exist_ok=True)
# Never accept a login prompt left by an earlier run.
if os.path.exists(env['RDCONS_DUMPFILE']): os.unlink(env['RDCONS_DUMPFILE'])
found=False
with open('build/init-run6.log','w') as out:
 p=subprocess.Popen([binary,root,'/sbin/init','--system'],env=env,stdout=out,stderr=out,start_new_session=True,preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
 open('build/init-test6.pid','w').write(str(p.pid))
 try:
  for i in range(480):
   time.sleep(.25)
   if p.poll() is not None:break
   if os.path.exists(env['RDCONS_DUMPFILE']) and 'login:' in open(env['RDCONS_DUMPFILE']).read():
    found=True;print('LOGIN prompt captured',flush=True)
    time.sleep(float(os.environ.get('RD_LOGIN_SETTLE_SECONDS','3')))
    listing=subprocess.check_output(['ps','-axo','pid=,command='],text=True)
    brokers=[line.strip() for line in listing.splitlines()
             if f'{binary} {root} /usr/bin/dbus-broker' in line]
    print('D-Bus broker processes after settle:',len(brokers),flush=True)
    break
 finally:
  # Services may create new sessions; stop only exact candidate/test-root processes.
  listing=subprocess.check_output(['ps','-axo','pid=,command='],text=True)
  for line in listing.splitlines():
   fields=line.strip().split(None,1)
   if len(fields)==2 and fields[1].startswith(f'{binary} {root} '):
    try:os.kill(int(fields[0]),signal.SIGKILL)
    except ProcessLookupError:pass
  try:p.wait(timeout=3)
  except subprocess.TimeoutExpired:p.kill();p.wait()
 print('init test PID',p.pid,'exit',p.returncode,flush=True)

sys.exit(0 if found else 1)
