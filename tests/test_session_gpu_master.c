/* Native, headless DRM-master fileport authorization regression. */
#include "../src/session_gpu_frame.h"
#include <CoreFoundation/CoreFoundation.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BGRA 0x42475241u

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int frames, losses;

static IOSurfaceRef surface(void) {
  int64_t values[] = {8, 8, BGRA, 4};
  const void *keys[] = {kIOSurfaceWidth, kIOSurfaceHeight,
                        kIOSurfacePixelFormat, kIOSurfaceBytesPerElement};
  const void *nums[4];
  for (int i = 0; i < 4; i++)
    nums[i] = CFNumberCreate(NULL, kCFNumberSInt64Type, &values[i]);
  CFDictionaryRef props = CFDictionaryCreate(NULL, keys, nums, 4,
      &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  IOSurfaceRef s = IOSurfaceCreate(props);
  CFRelease(props);
  for (int i = 0; i < 4; i++) CFRelease(nums[i]);
  return s;
}

static long present(IOSurfaceRef s, uint32_t crtc, uint64_t generation, void *ctx) {
  (void)ctx;
  assert(s && crtc == 1 && generation == 1);
  pthread_mutex_lock(&mu);
  frames++;
  pthread_cond_broadcast(&cv);
  pthread_mutex_unlock(&mu);
  return 0;
}

static void lost(void *ctx) {
  (void)ctx;
  pthread_mutex_lock(&mu);
  losses++;
  pthread_cond_broadcast(&cv);
  pthread_mutex_unlock(&mu);
}

static void wait_for(int *n, int target) {
  struct timespec limit;
  clock_gettime(CLOCK_REALTIME, &limit);
  limit.tv_sec += 4;
  pthread_mutex_lock(&mu);
  while (*n < target) assert(pthread_cond_timedwait(&cv, &mu, &limit) == 0);
  pthread_mutex_unlock(&mu);
}

static int count(void) {
  pthread_mutex_lock(&mu);
  int n = frames;
  pthread_mutex_unlock(&mu);
  return n;
}

static void run_child(int in, int out, const char *path, int device_fd, pid_t owner,
                      session_gpu_epoch epoch, int real_master) {
  IOSurfaceRef s = surface();
  if (!s) _exit(3);
  int fd = -1;
  char c;
  while (read(in, &c, 1) == 1) {
    if (c == 'q') break;
    int rc = -999;
    if (c == 'o') {
      fd = open(path, O_RDWR | O_NOFOLLOW);
      if (fd >= 0 && real_master && flock(fd, LOCK_EX | LOCK_NB) != 0) fd = -1;
      rc = fd >= 0 ? 0 : -1;
    } else if (c == 'r') {
      rc = session_gpu_child_register_master(owner, epoch, fd, device_fd);
    } else if (c == 'p') {
      rc = session_gpu_child_present_master(owner, epoch, fd, s, 1, 1, 8, 8, BGRA);
    } else if (c == 'h') {
      rc = session_gpu_child_present_device(owner, epoch, device_fd, s, 1, 1, 8, 8, BGRA);
    } else if (c == 'a') {
      rc = session_gpu_child_auth_device(owner, epoch, device_fd);
    } else if (c == 'x') {
      int impostor = socket(AF_UNIX, SOCK_DGRAM, 0);
      rc = session_gpu_child_auth_device(owner, epoch, impostor);
      close(impostor);
    } else if (c == 'n') {
      rc = session_gpu_child_present(owner, epoch, s, 1, 1, 8, 8, BGRA);
    } else if (c == 'u') {
      if (fd >= 0) { flock(fd, LOCK_UN); close(fd); }
      fd = open(path, O_RDWR | O_NOFOLLOW);
      rc = fd >= 0 ? 0 : -1;
    } else if (c == 'd') {
      int duplicate = dup(fd);
      if (duplicate >= 0) {
        flock(fd, LOCK_UN); close(fd);
        fd = duplicate; /* same inode/description, but explicitly unlocked */
        rc = 0;
      }
    }
    char result = (rc == 0) ? 'y' : 'n';
    if (write(out, &result, 1) != 1) _exit(4);
  }
  if (fd >= 0) close(fd);
  CFRelease(s);
  _exit(0);
}

static void command(int to, int from, char c, char expected) {
  assert(write(to, &c, 1) == 1);
  char result = 0;
  ssize_t n = read(from, &result, 1);
  if (n != 1 || result != expected)
    fprintf(stderr, "master command %c expected %c got %c n=%zd\n",
            c, expected, result, n);
  assert(n == 1 && result == expected);
}

int main(void) {
  char dir[] = "/tmp/shimmy-gpu-master-XXXXXX";
  assert(mkdtemp(dir));
  char path[256];
  snprintf(path, sizeof path, "%s/drm-master.lock", dir);
  int seed = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
  assert(seed >= 0); close(seed);
  session_gpu_epoch epoch = {(uint64_t)arc4random() << 32 | arc4random(),
                             (uint64_t)arc4random() << 32 | arc4random()};
  int master_in[2], master_out[2], stranger_in[2], stranger_out[2];
  int device_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  assert(device_fd >= 0);
  assert(pipe(master_in) == 0 && pipe(master_out) == 0 &&
         pipe(stranger_in) == 0 && pipe(stranger_out) == 0);
  pid_t owner = getpid();
  pid_t master = fork(); assert(master >= 0);
  if (!master) {
    close(master_in[1]); close(master_out[0]);
    close(stranger_in[0]); close(stranger_in[1]);
    close(stranger_out[0]); close(stranger_out[1]);
    run_child(master_in[0], master_out[1], path, device_fd, owner, epoch, 1);
  }
  pid_t stranger = fork(); assert(stranger >= 0);
  if (!stranger) {
    close(stranger_in[1]); close(stranger_out[0]);
    close(master_in[0]); close(master_in[1]);
    close(master_out[0]); close(master_out[1]);
    run_child(stranger_in[0], stranger_out[1], path, device_fd, owner, epoch, 0);
  }
  close(master_in[0]); close(master_out[1]);
  close(stranger_in[0]); close(stranger_out[1]);
  close(device_fd);
  assert(session_gpu_host_start_from_master(geteuid(), epoch, path,
                                            present, lost, NULL) == 0);
  command(stranger_in[1], stranger_out[0], 'o', 'y');
  command(stranger_in[1], stranger_out[0], 'a', 'n'); /* no registered master */
  command(stranger_in[1], stranger_out[0], 'r', 'n'); /* no lock */
  command(master_in[1], master_out[0], 'o', 'y');
  command(stranger_in[1], stranger_out[0], 'r', 'n'); /* same inode, other FD */
  command(master_in[1], master_out[0], 'r', 'y');
  command(stranger_in[1], stranger_out[0], 'a', 'y'); /* exact handed DRM fd */
  command(stranger_in[1], stranger_out[0], 'x', 'n'); /* unrelated socket */
  command(stranger_in[1], stranger_out[0], 'p', 'n'); /* audit+flock reject */
  command(stranger_in[1], stranger_out[0], 'h', 'y'); /* exact handed-off DRM fd */
  wait_for(&frames, 1);
  usleep(250000); assert(count() == 1);
  command(master_in[1], master_out[0], 'n', 'n'); /* no master right: reject */
  usleep(250000); assert(count() == 1);
  command(master_in[1], master_out[0], 'p', 'y');
  wait_for(&frames, 2);
  command(master_in[1], master_out[0], 'd', 'y'); /* copied then explicitly unlocked */
  command(master_in[1], master_out[0], 'r', 'n');
  command(master_in[1], master_out[0], 'p', 'n');
  command(stranger_in[1], stranger_out[0], 'a', 'n'); /* master unlocked */
  usleep(250000); assert(count() == 2);
  assert(write(master_in[1], "q", 1) == 1);
  int status;
  assert(waitpid(master, &status, 0) == master && WIFEXITED(status));
  wait_for(&losses, 2);
  assert(write(stranger_in[1], "q", 1) == 1);
  assert(waitpid(stranger, &status, 0) == stranger && WIFEXITED(status));
  session_gpu_host_stop();
  close(master_in[1]); close(master_out[0]);
  close(stranger_in[1]); close(stranger_out[0]);
  assert(unlink(path) == 0 && rmdir(dir) == 0);
  puts("GPU master proof: lock fileport, audit, copied/unlocked, same-inode impostor, loss passed");
  return 0;
}
