/* Native, headless two-process IOSurface frame transport regression. */
#include "../src/session_gpu_frame.h"
#include <CoreFoundation/CoreFoundation.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BGRA 0x42475241u

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_frames, g_losses;
static uint32_t g_crtc[4];
static uint64_t g_generation[4];

static long present(IOSurfaceRef surface, uint32_t crtc, uint64_t generation,
                    void *ctx) {
  (void)ctx;
  assert(surface && IOSurfaceGetWidth(surface) == 8 &&
         IOSurfaceGetHeight(surface) == 8);
  pthread_mutex_lock(&g_mu);
  assert(g_frames < 4);
  g_crtc[g_frames] = crtc;
  g_generation[g_frames] = generation;
  g_frames++;
  pthread_cond_broadcast(&g_cv);
  pthread_mutex_unlock(&g_mu);
  return 0;
}

static void lost(void *ctx) {
  (void)ctx;
  pthread_mutex_lock(&g_mu);
  g_losses++;
  pthread_cond_broadcast(&g_cv);
  pthread_mutex_unlock(&g_mu);
}

static void wait_at_least(int *value, int wanted) {
  struct timespec until;
  clock_gettime(CLOCK_REALTIME, &until);
  until.tv_sec += 4;
  pthread_mutex_lock(&g_mu);
  while (*value < wanted)
    assert(pthread_cond_timedwait(&g_cv, &g_mu, &until) == 0);
  pthread_mutex_unlock(&g_mu);
}

static int frames(void) {
  pthread_mutex_lock(&g_mu);
  int n = g_frames;
  pthread_mutex_unlock(&g_mu);
  return n;
}

static IOSurfaceRef make_surface(uint32_t format) {
  int64_t w = 8, h = 8, fmt = format, bpe = 4;
  const void *keys[4] = {kIOSurfaceWidth, kIOSurfaceHeight,
                         kIOSurfacePixelFormat, kIOSurfaceBytesPerElement};
  int64_t values[4] = {w, h, fmt, bpe};
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

static void child(int command_fd, int result_fd, pid_t owner,
                  session_gpu_epoch epoch) {
  IOSurfaceRef s = make_surface(BGRA);
  if (!s) _exit(2);
  char command;
  while (read(command_fd, &command, 1) == 1) {
    if (command == 'q') break;
    uint64_t gen = command == 'b' ? 2 : 1;
    uint32_t crtc = command == 'c' ? 2 : 1;
    uint32_t width = command == 'e' ? 9 : 8;
    uint32_t format = command == 'd' ? 0 : BGRA;
    IOSurfaceRef sent = command == 'f' ? make_surface(0x52474241u) : s;
    int rc = session_gpu_child_present(owner, epoch, sent, gen, crtc,
                                        width, 8, format);
    if (sent != s && sent) CFRelease(sent);
    /* Malformed format is rejected at the sender. Geometry mismatch reaches
     * the receiver and must be rejected there. */
    char result = command == 'd' ? (rc == -EINVAL ? 'y' : 'n') :
                  (rc == 0 ? 'y' : 'n');
    if (write(result_fd, &result, 1) != 1) _exit(3);
  }
  CFRelease(s);
  _exit(0);
}

static void command(int to, int from, char c) {
  assert(write(to, &c, 1) == 1);
  char result = 0;
  assert(read(from, &result, 1) == 1 && result == 'y');
}

int main(void) {
  session_gpu_epoch epoch = {(uint64_t)arc4random() << 32 | arc4random(),
                             (uint64_t)arc4random() << 32 | arc4random()};
  assert(epoch.high && epoch.low);
  int commands[2], results[2], stranger_commands[2], stranger_results[2];
  assert(pipe(commands) == 0 && pipe(results) == 0 &&
         pipe(stranger_commands) == 0 && pipe(stranger_results) == 0);
  pid_t owner = getpid();
  pid_t producer = fork();
  assert(producer >= 0);
  if (producer == 0) {
    close(commands[1]); close(results[0]);
    close(stranger_commands[0]); close(stranger_commands[1]);
    close(stranger_results[0]); close(stranger_results[1]);
    child(commands[0], results[1], owner, epoch);
  }
  pid_t stranger = fork();
  assert(stranger >= 0);
  if (stranger == 0) {
    close(stranger_commands[1]); close(stranger_results[0]);
    close(commands[0]); close(commands[1]);
    close(results[0]); close(results[1]);
    child(stranger_commands[0], stranger_results[1], owner, epoch);
  }
  close(commands[0]); close(results[1]);
  close(stranger_commands[0]); close(stranger_results[1]);
  assert(session_gpu_host_start(producer, geteuid(), epoch,
                                present, lost, NULL) == 0);
  command(commands[1], results[0], 'a');
  wait_at_least(&g_frames, 1);
  command(commands[1], results[0], 'a'); /* stale generation */
  usleep(250000);
  assert(frames() == 1);
  command(commands[1], results[0], 'e'); /* wrong IOSurface dimensions */
  usleep(250000);
  assert(frames() == 1);
  command(commands[1], results[0], 'd'); /* unsupported format */
  command(commands[1], results[0], 'f'); /* surface format mismatches metadata */
  usleep(250000);
  assert(frames() == 1);
  command(commands[1], results[0], 'b');
  wait_at_least(&g_frames, 2);
  command(commands[1], results[0], 'c');
  wait_at_least(&g_frames, 3);
  assert(g_crtc[0] == 1 && g_generation[0] == 1);
  assert(g_crtc[1] == 1 && g_generation[1] == 2);
  assert(g_crtc[2] == 2 && g_generation[2] == 1);

  command(stranger_commands[1], stranger_results[0], 'a');
  int status;
  usleep(250000);
  assert(frames() == 3);

  assert(session_gpu_host_set_producer(stranger, geteuid()) == 0);
  wait_at_least(&g_losses, 1); /* old graphics session cleared */
  command(stranger_commands[1], stranger_results[0], 'a');
  wait_at_least(&g_frames, 4); /* generation 1 valid in new producer epoch */

  assert(write(stranger_commands[1], "q", 1) == 1);
  assert(waitpid(stranger, &status, 0) == stranger && WIFEXITED(status));
  wait_at_least(&g_losses, 2);
  assert(write(commands[1], "q", 1) == 1); /* child exits without response */
  assert(waitpid(producer, &status, 0) == producer && WIFEXITED(status));
  session_gpu_host_stop();
  close(commands[1]); close(results[0]);
  close(stranger_commands[1]); close(stranger_results[0]);
  puts("GPU frame transport: rights, audit PID, generations, shape, two CRTCs, loss passed");
  return 0;
}
