// gpu_bridge.m — Metal bring-up, the Cocoa main-loop owner, and the guest hypercall demux.
//
// Split of responsibility (see gpu.h):
//   gpu_bridge.m  — the MTLDevice/MTLCommandQueue, gpu_init/gpu_hypercall/gpu_run_event_loop
//   window.m      — the NSWindow + CAMetalLayer and every gpuwin_* entry point
//
// gpu_init() deliberately creates *only* the device and queue: it runs long before the guest
// has asked for a window, and window.m pulls the device back out through gpu_metal_device().
// That accessor is dispatch_once-guarded, so any thread may be first to need it.
//
// gpu_run_event_loop() is the main thread's terminal call: -[NSApp run] never returns. The
// guest runs on a worker thread and _exit()s the process when it is done. Nothing may depend
// on gpuwin_poll() being pumped from here — NSApp run does not call it.
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include "gpu.h"

#include <errno.h>
#define GPU_ENOSYS (-38)   // Linux ENOSYS; the shim speaks raw Linux errno, not macOS errno
#define GPU_EINVAL (-22)
#define GPU_EFAULT (-14)

// ---------------------------------------------------------------- device / queue
static id<MTLDevice>       gDev;
static id<MTLCommandQueue> gQueue;

static void gpu_bringup(void) {
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    gDev = MTLCreateSystemDefaultDevice();
    if (!gDev) { fprintf(stderr, "[gpu] no Metal device\n"); return; }
    gQueue = [gDev newCommandQueue];
    if (!gQueue) { fprintf(stderr, "[gpu] no command queue\n"); return; }
    gQueue.label = @"r-drift.linux-shim";
    fprintf(stderr, "[gpu] Metal device: %s\n", gDev.name.UTF8String);
  });
}

void gpu_init(void) { gpu_bringup(); }

// Accessors used by window.m. Safe from any thread and before any window exists.
id<MTLDevice>       gpu_metal_device(void) { gpu_bringup(); return gDev; }
id<MTLCommandQueue> gpu_metal_queue(void)  { gpu_bringup(); return gQueue; }

// ---------------------------------------------------------------- hypercall demux
// op families:  0x1000 compute (later)   0x2000 window   0x3000 GL (later)
enum {
  GPU_FAMILY_MASK = 0xF000,
  GPU_FAMILY_COMPUTE = 0x1000,
  GPU_FAMILY_WINDOW  = 0x2000,
  GPU_FAMILY_GL      = 0x3000,

  GPU_WIN_OPEN    = 0x2000,   // a0=w, a1=h
  GPU_WIN_PRESENT = 0x2001,   // a0=rgba*, a1=w, a2=h
  GPU_WIN_POLL    = 0x2002,   // result = !=0 when the user closed the window
  GPU_WIN_CURSOR  = 0x2003,   // a0=int* x, a1=int* y  (window-local, top-left origin)
};

// Guest pointers in a0..a3 are real host addresses — the guest shares our address space, so
// they are dereferenced directly with no translation.
long gpu_hypercall(struct gpu_req* r) {
  if (!r) return GPU_EFAULT;

  switch (r->op & GPU_FAMILY_MASK) {
  case GPU_FAMILY_WINDOW:
    switch (r->op) {
    case GPU_WIN_OPEN: {
      long rc = gpuwin_open(r->a0, r->a1);
      r->result = (uint64_t)rc;
      return rc;
    }
    case GPU_WIN_PRESENT: {
      const uint8_t* fb = (const uint8_t*)(uintptr_t)r->a0;
      if (!fb) return GPU_EFAULT;
      long rc = gpuwin_present(fb, r->a1, r->a2);
      r->result = (uint64_t)rc;
      return rc;
    }
    case GPU_WIN_POLL: {
      int closed = gpuwin_poll();
      r->result = (uint64_t)(uint32_t)closed;
      return 0;
    }
    case GPU_WIN_CURSOR: {
      int x = 0, y = 0;
      gpuwin_cursor(&x, &y);
      if (r->a0) *(int*)(uintptr_t)r->a0 = x;
      if (r->a1) *(int*)(uintptr_t)r->a1 = y;
      r->result = ((uint64_t)(uint32_t)x) | (((uint64_t)(uint32_t)y) << 32);
      return 0;
    }
    default:
      return GPU_ENOSYS;
    }

  case GPU_FAMILY_COMPUTE:   // not wired up yet
  case GPU_FAMILY_GL:        // GL->Metal bridge lands with glxgears
  default:
    return GPU_ENOSYS;
  }
}

// ---------------------------------------------------------------- main loop
// Main thread only. Never returns.
void gpu_run_event_loop(void) {
  [NSApplication sharedApplication];
  // Non-bundled binary: without an explicit regular policy the process stays a background
  // agent and its window never comes to the front.
  [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
  [NSApp activateIgnoringOtherApps:YES];
  [NSApp run];
}
