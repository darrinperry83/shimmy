// Synthetic two-process DRM flip transport. No AppKit, guest root, or display access.
#include "session_frame.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int shim_fd_reserve(int fd) { return fd; }
void shim_fd_unreserve(int fd) { (void)fd; }

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int count, losses;
static uint32_t outputs[8];
static uint8_t samples[8];

static long present(const uint8_t* data, uint32_t w, uint32_t h, uint32_t crtc) {
  assert(data && w == 8 && h == 8 && (crtc == 1 || crtc == 2));
  for (size_t i=1; i<(size_t)w*h*4; i++) assert(data[i] == data[0]);
  pthread_mutex_lock(&mu);
  if (count < 8) { outputs[count] = crtc; samples[count] = data[0]; }
  count++;
  pthread_cond_broadcast(&cv);
  pthread_mutex_unlock(&mu);
  return 0;
}

static void lost(void) {
  pthread_mutex_lock(&mu);
  losses++;
  pthread_cond_broadcast(&cv);
  pthread_mutex_unlock(&mu);
}

static void wait_for(int* value, int target) {
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += 3;
  pthread_mutex_lock(&mu);
  while (*value < target) assert(pthread_cond_timedwait(&cv, &mu, &deadline) == 0);
  pthread_mutex_unlock(&mu);
}

static void child_send(uint8_t a, uint8_t b, int has_server) {
  uint8_t frame[8*8*4];
  memset(frame,a,sizeof frame);
  long rc = session_frame_child_present(frame,8,8,1);
  if (!has_server) _exit(rc == -ENODEV ? 0 : 12);
  if (rc != 0) _exit(10);
  // Wait for parent to observe the first frame before producing another one.
  usleep(300000);
  if (b) {
    memset(frame,b,sizeof frame);
    if (session_frame_child_present(frame,8,8,2) != 0) _exit(11);
    usleep(300000);
  }
  _exit(0);
}

static pid_t spawn(uint8_t a, uint8_t b, int has_server) {
  pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) child_send(a,b,has_server);
  return pid;
}

static void reap(pid_t pid) {
  int status;
  assert(waitpid(pid,&status,0) == pid);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(void) {
  char root[32], dir[128];
  snprintf(root,sizeof root,"%d",(int)getpid());
  snprintf(dir,sizeof dir,"/tmp/rdx-%s",root);
  assert(mkdir(dir,0700) == 0);
  assert(setenv("RDVT_ROOT_PID",root,1) == 0);
  assert(session_frame_host_start_with_lost(present,lost) == 0);
  pid_t first = spawn(11,22,1);
  wait_for(&count,2);
  assert(outputs[0] == 1 && samples[0] == 11);
  assert(outputs[1] == 2 && samples[1] == 22);
  reap(first);
  wait_for(&losses,1); // dead producer cannot leave its last frame visible
  pid_t second = spawn(33,0,1);
  wait_for(&count,3);
  assert(outputs[2] == 1 && samples[2] == 33);
  reap(second);
  wait_for(&losses,2);
  session_frame_host_stop();
  pid_t failed = spawn(44,0,0);
  reap(failed);
  DIR* d = opendir(dir);
  assert(d);
  struct dirent* e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    char path[256];
    snprintf(path,sizeof path,"%s/%s",dir,e->d_name);
    assert(unlink(path) == 0);
  }
  closedir(d);
  assert(rmdir(dir) == 0);
  puts("frame bridge: two outputs, replacement, loss, and failed send passed");
  return 0;
}
