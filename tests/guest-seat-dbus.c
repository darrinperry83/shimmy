// Guest-side diagnostic for the authenticated login test. Build against the Arch guest's
// libsystemd headers/library, copy the AArch64 result to private-root/tmp/shim-seat-dbus,
// then run RD_TEST_SEAT_DBUS=1 tests/run-interactive-login.py. Never use a live root.
#include <systemd/sd-bus.h>
#include <systemd/sd-device.h>
#include <sys/sysmacros.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
int main(void) {
 sd_device *event=NULL; const char *node=NULL;
 int r=sd_device_new_from_devnum(&event,'c',makedev(13,64));
 printf("device r=%d\n",r);
 if(r>=0) { r=sd_device_get_devname(event,&node); printf("devname r=%d name=%s\n",r,node?node:"-"); }
 if(node) {int fd=open(node,O_RDWR|O_CLOEXEC|O_NOCTTY|O_NONBLOCK); printf("direct logind-flags open fd=%d errno=%d\n",fd,errno); if(fd>=0) close(fd);}
 sd_device_unref(event);
 sd_bus *bus=NULL; sd_bus_message *reply=NULL; sd_bus_error err=SD_BUS_ERROR_NULL;
 r=sd_bus_open_system(&bus); printf("open bus r=%d\n",r); fflush(stdout); if(r<0) return 1;
 const char *dest="org.freedesktop.login1", *path="/org/freedesktop/login1/session/c1", *iface="org.freedesktop.login1.Session";
 r=sd_bus_call_method(bus,dest,path,iface,"TakeControl",&err,&reply,"b",0);
 printf("TakeControl r=%d error=%s message=%s\n",r,err.name?err.name:"-",err.message?err.message:"-"); fflush(stdout);
 sd_bus_message_unref(reply); reply=NULL; sd_bus_error_free(&err); if(r<0) return 2;
 for(int i=0;i<2;i++) {
  unsigned major=i?226:13,minor=i?0:64;
  r=sd_bus_call_method(bus,dest,path,iface,"TakeDevice",&err,&reply,"uu",major,minor);
  printf("TakeDevice %u:%u r=%d error=%s message=%s\n",major,minor,r,err.name?err.name:"-",err.message?err.message:"-"); fflush(stdout);
  if(r>=0) {int fd=-1,paused=-1; r=sd_bus_message_read(reply,"hb",&fd,&paused); printf("ReadReply %u:%u r=%d fd=%d paused=%d\n",major,minor,r,fd,paused); fflush(stdout);}
  sd_bus_message_unref(reply); reply=NULL; sd_bus_error_free(&err);
 }
 sd_bus_unref(bus); return 0;
}
