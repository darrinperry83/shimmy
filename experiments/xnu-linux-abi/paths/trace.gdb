set pagination off
set confirm off
set startup-with-shell off
python
import gdb
rd_enter = True
class RdPathTrace(gdb.Command):
 def __init__(self): super().__init__('rd-path-trace',gdb.COMMAND_USER)
 def invoke(self,arg,tty):
  global rd_enter
  nr=int(gdb.parse_and_eval('$x8')); args=[int(gdb.parse_and_eval('$x'+str(i))) & ((1<<64)-1) for i in range(6)]
  if rd_enter:
   path=''
   if nr in (56,78,79,291):
    try:
     raw=bytearray()
     for i in range(512):
      ch=bytes(gdb.selected_inferior().read_memory(args[1]+i,1))[0]
      if not ch:break
      raw.append(ch)
     path=repr(raw.decode(errors='replace'))
    except Exception:path='<unreadable>'
   print('PATH_ENTER nr=%d args=%s path=%s'%(nr,','.join(hex(a) for a in args),path))
  else:
   value=args[0] if args[0]<(1<<63) else args[0]-(1<<64)
   print('PATH_RETURN nr=%d result=%d'%(nr,value))
  rd_enter=not rd_enter
RdPathTrace()
end
catch syscall openat readlinkat newfstatat statx fcntl
commands
silent
rd-path-trace
continue
end
run --inhibit-cache --library-path /capture/rootfs/usr/lib/systemd:/capture/rootfs/usr/lib /capture/rootfs/usr/lib/systemd/systemd --version
