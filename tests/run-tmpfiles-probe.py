"""Bounded isolated tmpfiles probe. Run only while login-root is exclusively idle."""
import os, subprocess, resource, signal
base=os.getcwd()
env=os.environ.copy()
env.update(RDUID0='1', RDSOCKETGROUP='/tmp/rd-tmpfiles-probe', RDNETSOCK='1', RDNOTIFYDBG='1', RDPOLLLOG='1', RDTIMERFD_TRACE='1', SYSTEMD_LOG_LEVEL='debug', SYSTEMD_LOG_TARGET='console')
os.makedirs(env['RDSOCKETGROUP'],exist_ok=True)
with open('build/tmpfiles-probe.log','w') as out:
 p=subprocess.Popen(['./shim_cli_login_candidate','tests/login-root','/usr/bin/systemd-tmpfiles','--prefix=/dev','--create','--boot','--graceful'],env=env,stdout=out,stderr=out,start_new_session=True,preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
 print('tmpfiles PID',p.pid,flush=True)
 try:
  rc=p.wait(timeout=20)
  print('exit',rc,flush=True)
 except subprocess.TimeoutExpired:
  print('TIMEOUT after 20s',flush=True)
 finally:
  if p.poll() is None:
   os.killpg(p.pid,signal.SIGKILL)
   p.wait()
