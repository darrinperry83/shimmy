// test_window.m — standalone bring-up test for window.m + gpu_bridge.m.
//
// Build:
//   clang -arch arm64 -O2 -ObjC -fobjc-arc -o test_window \
//         test_window.m window.m gpu_bridge.m \
//         -framework Metal -framework MetalKit -framework Cocoa \
//         -framework QuartzCore -framework CoreGraphics
//
// Run: ./test_window [seconds]      (default 10)
//
// What it proves:
//  * gpu_init() brings Metal up with no window in existence,
//  * gpuwin_open() is idempotent (called twice; one window must appear),
//  * gpuwin_present() works from a NON-main worker thread (the real caller is the X server
//    thread) while main pumps the run loop — i.e. the dispatch_sync marshalling does not
//    deadlock,
//  * gpuwin_cursor() returns sane window-local top-left coords from both threads.
//
// The image is deliberately unmistakable: a moving R/G gradient with a pulsing blue channel,
// a moving filled white/yellow circle, a RED square in the TOP-LEFT corner and a BLUE square
// in the BOTTOM-RIGHT corner. The corner markers make a vertical flip impossible to miss.
#import <Cocoa/Cocoa.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "gpu.h"

#define W 640
#define H 480
#define MARK 48

static uint8_t g_fb[W * H * 4];
static atomic_int g_done = 0;
static atomic_int g_frames = 0;
static atomic_int g_errs = 0;
static int g_total_frames = 300;

// Guest-style RGBA8: byte0=R, byte1=G, byte2=B, byte3=A. Row 0 is the TOP row.
static void render(int f) {
  double t = f * 0.06;
  double cx = W * 0.5 + sin(t) * (W * 0.28);
  double cy = H * 0.5 + cos(t * 1.3) * (H * 0.28);
  double rad = 70.0 + 18.0 * sin(t * 2.0);
  double r2 = rad * rad;
  uint8_t blue = (uint8_t)(127.5 + 127.0 * sin(t));

  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      uint8_t* p = &g_fb[(y * W + x) * 4];
      p[0] = (uint8_t)(x * 255 / (W - 1));
      p[1] = (uint8_t)(y * 255 / (H - 1));
      p[2] = blue;
      p[3] = 255;

      double dx = x - cx, dy = y - cy;
      if (dx * dx + dy * dy <= r2) {          // filled circle, white core / yellow rim
        double d = sqrt(dx * dx + dy * dy) / rad;
        p[0] = 255;
        p[1] = 255;
        p[2] = (uint8_t)(255.0 * (1.0 - d));
      }
    }
  }
  // Orientation markers.
  for (int y = 0; y < MARK; y++)
    for (int x = 0; x < MARK; x++) {
      uint8_t* p = &g_fb[(y * W + x) * 4];             // top-left -> RED
      p[0] = 255; p[1] = 0; p[2] = 0; p[3] = 255;
      uint8_t* q = &g_fb[((H - 1 - y) * W + (W - 1 - x)) * 4];  // bottom-right -> BLUE
      q[0] = 0; q[1] = 0; q[2] = 255; q[3] = 255;
    }
}

// The "X server" thread: renders and presents; never touches Cocoa directly.
static void* worker(void* arg) {
  (void)arg;
  for (int f = 0; f < g_total_frames && !atomic_load(&g_done); f++) {
    render(f);
    long rc = gpuwin_present(g_fb, W, H);
    if (rc < 0) {
      atomic_fetch_add(&g_errs, 1);
      if (atomic_load(&g_errs) <= 3) fprintf(stderr, "[test] present rc=%ld (frame %d)\n", rc, f);
    } else {
      atomic_fetch_add(&g_frames, 1);
    }
    if (f == 30) {
      int x = -1, y = -1;
      gpuwin_cursor(&x, &y);
      printf("[test] cursor from WORKER thread after 30 frames: (%d, %d)\n", x, y);
      fflush(stdout);
    }
    usleep(33000);
  }
  atomic_store(&g_done, 1);
  return NULL;
}

int main(int argc, char** argv) {
  double secs = (argc > 1) ? atof(argv[1]) : 10.0;
  if (secs < 1.0) secs = 1.0;
  g_total_frames = (int)(secs * 30.0);

  printf("[test] gpu_init() with no window yet...\n");
  gpu_init();

  int cx0 = -1, cy0 = -1;
  gpuwin_cursor(&cx0, &cy0);
  printf("[test] cursor BEFORE any window: (%d, %d)  [expect 0,0 - no size known]\n", cx0, cy0);

  long rc = gpuwin_open(W, H);
  printf("[test] gpuwin_open(%d,%d) -> %ld\n", W, H, rc);
  if (rc < 0) return 1;

  int cx1 = -1, cy1 = -1;
  gpuwin_cursor(&cx1, &cy1);
  printf("[test] cursor after open, BEFORE first present: (%d, %d)  [expect window centre %d,%d]\n",
         cx1, cy1, W / 2, H / 2);

  rc = gpuwin_open(W, H);           // idempotency: must resize, not spawn a second window
  printf("[test] gpuwin_open() again (idempotency) -> %ld\n", rc);

  pthread_t th;
  if (pthread_create(&th, NULL, worker, NULL) != 0) { perror("pthread_create"); return 1; }

  printf("[test] presenting ~%d frames from a worker thread for %.1fs...\n", g_total_frames, secs);
  fflush(stdout);

  // Main thread: pump Cocoa. This also drains the main dispatch queue, which is what lets the
  // worker's dispatch_sync(present) complete.
  while (!atomic_load(&g_done)) {
    @autoreleasepool {
      if (gpuwin_poll()) {
        printf("[test] window closed by user\n");
        atomic_store(&g_done, 1);
        break;
      }
    }
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.005, true);
  }
  pthread_join(th, NULL);

  int mx = -1, my = -1;
  gpuwin_cursor(&mx, &my);
  printf("[test] frames presented ok: %d, errors: %d\n",
         atomic_load(&g_frames), atomic_load(&g_errs));
  printf("[test] cursor from MAIN thread at end: (%d, %d)\n", mx, my);
  NSPoint m = [NSEvent mouseLocation];
  printf("[test] (raw screen mouseLocation was %.1f, %.1f, bottom-left origin)\n", m.x, m.y);
  printf("[test] gpuwin_poll() closed flag = %d\n", gpuwin_poll());
  return 0;
}
