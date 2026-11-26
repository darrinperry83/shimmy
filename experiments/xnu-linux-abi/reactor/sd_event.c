/* Custom callback application linked against the unchanged Arch libsystemd. */
#include <systemd/sd-event.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
static unsigned fired;
static void finish(sd_event_source *s){if(fired==7)sd_event_exit(sd_event_source_get_event(s),0);}
static int io_event(sd_event_source *s,int fd,uint32_t events,void *userdata){
 (void)userdata;uint64_t count=0;if(events!=EPOLLIN||read(fd,&count,8)!=8||count!=1)return -1;
 fired|=1;if(kill(getpid(),SIGUSR1))return -1;finish(s);return 0;
}
static int signal_event(sd_event_source *s,const struct signalfd_siginfo *si,void *userdata){
 (void)userdata;if(si->ssi_signo!=SIGUSR1||si->ssi_pid!=(uint32_t)getpid())return -1;
 fired|=2;finish(s);return 0;
}
static int time_event(sd_event_source *s,uint64_t usec,void *userdata){
 (void)usec;(void)userdata;fired|=4;finish(s);return 0;
}
int main(void){
 sigset_t set;sigemptyset(&set);sigaddset(&set,SIGUSR1);if(sigprocmask(SIG_BLOCK,&set,NULL))return 1;
 sd_event *event=NULL;sd_event_source *io=NULL,*signal=NULL,*timer=NULL;
 int fd=eventfd(1,EFD_NONBLOCK|EFD_CLOEXEC);if(fd<0)return 1;
 int r=sd_event_new(&event);if(r<0){fprintf(stderr,"sd_event_new: %d\n",r);return 1;}
 r=sd_event_add_io(event,&io,fd,EPOLLIN,io_event,NULL);if(r<0){fprintf(stderr,"sd_event_add_io: %d\n",r);return 1;}
 r=sd_event_source_set_enabled(io,SD_EVENT_ONESHOT);if(r<0){fprintf(stderr,"sd_event_oneshot: %d\n",r);return 1;}
 r=sd_event_add_signal(event,&signal,SIGUSR1,signal_event,NULL);if(r<0){fprintf(stderr,"sd_event_add_signal: %d\n",r);return 1;}
 struct timespec now;if(clock_gettime(CLOCK_MONOTONIC,&now))return 1;
 uint64_t deadline=(uint64_t)now.tv_sec*1000000+now.tv_nsec/1000+50000;
 r=sd_event_add_time(event,&timer,CLOCK_MONOTONIC,deadline,0,time_event,NULL);if(r<0){fprintf(stderr,"sd_event_add_time: %d\n",r);return 1;}
 r=sd_event_loop(event);if(r<0){fprintf(stderr,"sd_event_loop: %d\n",r);return 1;}
 sd_event_source_unref(timer);sd_event_source_unref(signal);sd_event_source_unref(io);sd_event_unref(event);close(fd);
 if(r||fired!=7)return 1;puts("ARCH_SD_EVENT_LOOP_PASS: original libsystemd IO oneshot, signal and timer callbacks");return 0;
}
