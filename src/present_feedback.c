#include "present_feedback.h"
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>

/* A late handler cannot write a recycled descriptor or acknowledge a new run. */
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int wake_pipe[2]={-1,-1};
static uint32_t generation;
static uint32_t submitted[RD_FEEDBACK_OUTPUTS];
static uint64_t completed[RD_FEEDBACK_OUTPUTS];

int rd_feedback_init(void){
  pthread_mutex_lock(&lock);
  if(wake_pipe[0]>=0){pthread_mutex_unlock(&lock);return 0;}
  if(generation==UINT32_MAX){pthread_mutex_unlock(&lock);return -1;}
  int fds[2];
  if(pipe(fds)){pthread_mutex_unlock(&lock);return -1;}
  for(int i=0;i<2;i++){
    int flags=fcntl(fds[i],F_GETFL,0);
    if(flags<0||fcntl(fds[i],F_SETFL,flags|O_NONBLOCK)<0){
      close(fds[0]);close(fds[1]);pthread_mutex_unlock(&lock);return -1;
    }
  }
  wake_pipe[0]=fds[0];wake_pipe[1]=fds[1];generation++;
  for(unsigned o=0;o<RD_FEEDBACK_OUTPUTS;o++){
    submitted[o]=0;completed[o]=0;
  }
  pthread_mutex_unlock(&lock);
  return 0;
}
int rd_feedback_fd(void){
  pthread_mutex_lock(&lock);int fd=wake_pipe[0];pthread_mutex_unlock(&lock);
  return fd;
}
uint64_t rd_feedback_submit(unsigned output){
  pthread_mutex_lock(&lock);
  uint64_t seq=0;
  if(output<RD_FEEDBACK_OUTPUTS&&wake_pipe[1]>=0&&submitted[output]<UINT32_MAX)
    seq=((uint64_t)generation<<32)|++submitted[output];
  pthread_mutex_unlock(&lock);
  return seq;
}
uint64_t rd_feedback_submitted(unsigned output){
  pthread_mutex_lock(&lock);
  uint64_t seq=output<RD_FEEDBACK_OUTPUTS&&wake_pipe[1]>=0&&submitted[output]
    ?((uint64_t)generation<<32)|submitted[output]:0;
  pthread_mutex_unlock(&lock);
  return seq;
}
void rd_feedback_finish(unsigned output,uint64_t sequence){
  pthread_mutex_lock(&lock);
  if(output<RD_FEEDBACK_OUTPUTS&&wake_pipe[1]>=0&&
     (sequence>>32)==generation&&(uint32_t)sequence&&
     (uint32_t)sequence<=submitted[output]&&sequence>completed[output]){
    completed[output]=sequence;
    char byte=1;(void)write(wake_pipe[1],&byte,1);
  }
  pthread_mutex_unlock(&lock);
}
uint64_t rd_feedback_completed(unsigned output){
  pthread_mutex_lock(&lock);
  uint64_t seq=output<RD_FEEDBACK_OUTPUTS&&wake_pipe[0]>=0?completed[output]:0;
  pthread_mutex_unlock(&lock);
  return seq;
}
void rd_feedback_drain(void){
  pthread_mutex_lock(&lock);
  if(wake_pipe[0]>=0){char buf[128];while(read(wake_pipe[0],buf,sizeof buf)>0){}}
  pthread_mutex_unlock(&lock);
}
void rd_feedback_close(void){
  pthread_mutex_lock(&lock);
  for(int i=0;i<2;i++){
    if(wake_pipe[i]>=0)close(wake_pipe[i]);
    wake_pipe[i]=-1;
  }
  pthread_mutex_unlock(&lock);
}
