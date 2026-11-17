/* Completed IOSurface frame transport: compositor host process -> persistent
 * GUI owner. No pixel mapping or CPU copy occurs on this path. */
#include "session_gpu_frame.h"

#include <bsm/libbsm.h>
#include <errno.h>
#include <mach/mach.h>
#include <pthread.h>
#include <servers/bootstrap.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define GPU_FRAME_MAGIC 0x53474631u
#define GPU_FRAME_MSG_ID 0x53474631
#define GPU_FRAME_VERSION 1u
#define GPU_FRAME_MAX_SIDE 16384u
#define GPU_FRAME_BGRA 0x42475241u
#define GPU_FRAME_POLL_MS 200u

typedef struct {
  mach_msg_header_t header;
  mach_msg_body_t body;
  mach_msg_port_descriptor_t port;
  uint32_t magic, version;
  uint64_t epoch_high, epoch_low, generation;
  uint32_t claimed_pid, crtc, width, height, format;
} gpu_frame_message;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
/* Serializes presentation with producer replacement and VT-clear callbacks.
 * Callbacks must not reenter the host lifecycle API. */
static pthread_mutex_t g_delivery_lock = PTHREAD_MUTEX_INITIALIZER;
static mach_port_t g_recv_port = MACH_PORT_NULL;
static pthread_t g_thread;
static int g_running;
static pid_t g_expected_pid;
static uid_t g_expected_uid;
static session_gpu_epoch g_epoch;
static uint64_t g_last_generation[2];
static int g_has_frame[2];
static session_gpu_present_fn g_present;
static session_gpu_lost_fn g_lost;
static void *g_ctx;

static int service_name(char *out, size_t cap, pid_t owner, session_gpu_epoch epoch) {
  if (owner <= 1 || !epoch.high || !epoch.low) return -EINVAL;
  int n = snprintf(out, cap, "com.rdx.sessiongpu.%d.%016llx%016llx",
                   owner, (unsigned long long)epoch.high,
                   (unsigned long long)epoch.low);
  return n > 0 && (size_t)n < cap ? 0 : -ENAMETOOLONG;
}

static int valid_geometry(uint32_t crtc, uint32_t w, uint32_t h, uint32_t format) {
  return (crtc == 1 || crtc == 2) && w && h &&
         w <= GPU_FRAME_MAX_SIDE && h <= GPU_FRAME_MAX_SIDE &&
         (uint64_t)w * h * 4 <= 256ull * 1024 * 1024 &&
         format == GPU_FRAME_BGRA;
}

static int receive_one(mach_msg_header_t *hdr, size_t cap) {
  if (hdr->msgh_size != sizeof(gpu_frame_message) ||
      hdr->msgh_size + sizeof(mach_msg_audit_trailer_t) > cap ||
      hdr->msgh_id != GPU_FRAME_MSG_ID ||
      !(hdr->msgh_bits & MACH_MSGH_BITS_COMPLEX)) return -EBADMSG;
  gpu_frame_message *m = (gpu_frame_message *)hdr;
  if (m->body.msgh_descriptor_count != 1 ||
      m->port.type != MACH_MSG_PORT_DESCRIPTOR ||
      m->port.name == MACH_PORT_NULL ||
      m->magic != GPU_FRAME_MAGIC || m->version != GPU_FRAME_VERSION ||
      !valid_geometry(m->crtc, m->width, m->height, m->format) ||
      !m->generation) return -EBADMSG;

  mach_msg_audit_trailer_t *trailer = (mach_msg_audit_trailer_t *)
      ((uint8_t *)hdr + round_msg(hdr->msgh_size));
  if (trailer->msgh_trailer_type != MACH_MSG_TRAILER_FORMAT_0 ||
      trailer->msgh_trailer_size < sizeof(*trailer)) return -EACCES;
  pid_t sender = audit_token_to_pid(trailer->msgh_audit);
  uid_t sender_uid = audit_token_to_euid(trailer->msgh_audit);

  pthread_mutex_lock(&g_lock);
  int authorized = g_running && g_expected_pid > 1 &&
      sender == g_expected_pid && sender_uid == g_expected_uid &&
      m->claimed_pid == (uint32_t)sender &&
      m->epoch_high == g_epoch.high && m->epoch_low == g_epoch.low;
  int slot = (int)m->crtc - 1;
  if (authorized && m->generation <= g_last_generation[slot]) authorized = 0;
  pthread_mutex_unlock(&g_lock);
  if (!authorized) return -EACCES;

  IOSurfaceRef surface = IOSurfaceLookupFromMachPort(m->port.name);
  if (!surface) return -EINVAL;
  int shape = IOSurfaceGetWidth(surface) == m->width &&
      IOSurfaceGetHeight(surface) == m->height &&
      IOSurfaceGetPixelFormat(surface) == m->format &&
      IOSurfaceGetBytesPerElement(surface) == 4;
  if (!shape) { CFRelease(surface); return -EINVAL; }

  /* A new session may be authorized while the surface is being resolved. */
  pthread_mutex_lock(&g_delivery_lock);
  pthread_mutex_lock(&g_lock);
  authorized = g_running && sender == g_expected_pid &&
      sender_uid == g_expected_uid &&
      m->epoch_high == g_epoch.high && m->epoch_low == g_epoch.low &&
      m->generation > g_last_generation[slot];
  session_gpu_present_fn present = authorized ? g_present : NULL;
  void *ctx = g_ctx;
  pthread_mutex_unlock(&g_lock);
  int rc = present ? (int)present(surface, m->crtc, m->generation, ctx) : -EACCES;
  if (rc == 0) {
    pthread_mutex_lock(&g_lock);
    g_last_generation[slot] = m->generation;
    g_has_frame[slot] = 1;
    pthread_mutex_unlock(&g_lock);
  }
  pthread_mutex_unlock(&g_delivery_lock);
  CFRelease(surface);
  return rc;
}

static void *receive_loop(void *arg) {
  (void)arg;
  while (1) {
    pthread_mutex_lock(&g_lock);
    int running = g_running;
    mach_port_t port = g_recv_port;
    pid_t producer = g_expected_pid;
    pthread_mutex_unlock(&g_lock);
    if (!running) break;

    /* Large enough for the fixed message plus the kernel audit trailer. */
    _Alignas(8) uint8_t buffer[sizeof(gpu_frame_message) +
                               sizeof(mach_msg_audit_trailer_t) + 64];
    memset(buffer, 0, sizeof buffer);
    mach_msg_header_t *hdr = (mach_msg_header_t *)buffer;
    kern_return_t kr = mach_msg(hdr, MACH_RCV_MSG | MACH_RCV_TIMEOUT |
                               MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AUDIT) |
                               MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0),
                               0, sizeof buffer, port, GPU_FRAME_POLL_MS,
                               MACH_PORT_NULL);
    if (kr == KERN_SUCCESS) {
      (void)receive_one(hdr, sizeof buffer);
      mach_msg_destroy(hdr); /* always discard the received send-right */
    }
    if (producer > 1 && kill(producer, 0) < 0 && errno == ESRCH) {
      pthread_mutex_lock(&g_delivery_lock);
      pthread_mutex_lock(&g_lock);
      int lost = g_running && g_expected_pid == producer &&
          (g_has_frame[0] || g_has_frame[1]);
      if (lost) {
        g_expected_pid = 0;
        memset(g_has_frame, 0, sizeof g_has_frame);
        memset(g_last_generation, 0, sizeof g_last_generation);
      }
      session_gpu_lost_fn callback = lost ? g_lost : NULL;
      void *ctx = g_ctx;
      pthread_mutex_unlock(&g_lock);
      if (callback) callback(ctx);
      pthread_mutex_unlock(&g_delivery_lock);
    }
  }
  return NULL;
}

int session_gpu_host_start(pid_t expected_pid, uid_t expected_uid,
                           session_gpu_epoch epoch,
                           session_gpu_present_fn present,
                           session_gpu_lost_fn lost, void *ctx) {
  if (expected_pid <= 1 || !present || getpid() <= 1) return -EINVAL;
  char name[BOOTSTRAP_MAX_NAME_LEN];
  int rc = service_name(name, sizeof name, getpid(), epoch);
  if (rc) return rc;
  pthread_mutex_lock(&g_lock);
  if (g_running) { pthread_mutex_unlock(&g_lock); return -EALREADY; }
  pthread_mutex_unlock(&g_lock);

  mach_port_t port = MACH_PORT_NULL;
  if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port) != KERN_SUCCESS)
    return -EIO;
  if (mach_port_insert_right(mach_task_self(), port, port,
                             MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS) {
    mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1);
    return -EIO;
  }
  mach_port_t bp = MACH_PORT_NULL;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  kern_return_t bootstrap_rc = task_get_bootstrap_port(mach_task_self(), &bp);
  kern_return_t register_rc = bootstrap_rc == KERN_SUCCESS ?
      bootstrap_register(bp, name, port) : bootstrap_rc;
  int registered = register_rc == BOOTSTRAP_SUCCESS;
#pragma clang diagnostic pop
  if (!registered) {
    fprintf(stderr, "session_gpu: bootstrap registration failed name=%s kr=%d\n",
            name, register_rc);
    mach_port_deallocate(mach_task_self(), port);
    mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1);
    return -EIO;
  }

  pthread_mutex_lock(&g_lock);
  g_recv_port = port;
  g_expected_pid = expected_pid;
  g_expected_uid = expected_uid;
  g_epoch = epoch;
  g_present = present;
  g_lost = lost;
  g_ctx = ctx;
  memset(g_last_generation, 0, sizeof g_last_generation);
  memset(g_has_frame, 0, sizeof g_has_frame);
  g_running = 1;
  pthread_mutex_unlock(&g_lock);
  if (pthread_create(&g_thread, NULL, receive_loop, NULL) != 0) {
    /* A bootstrap-registered receive right cannot be safely reused. */
    session_gpu_host_stop();
    return -EIO;
  }
  return 0;
}

int session_gpu_host_set_producer(pid_t expected_pid, uid_t expected_uid) {
  if (expected_pid <= 1) return -EINVAL;
  pthread_mutex_lock(&g_delivery_lock);
  pthread_mutex_lock(&g_lock);
  if (!g_running) {
    pthread_mutex_unlock(&g_lock);
    pthread_mutex_unlock(&g_delivery_lock);
    return -ENODEV;
  }
  int changed = g_expected_pid != expected_pid || g_expected_uid != expected_uid;
  int had_frame = g_has_frame[0] || g_has_frame[1];
  session_gpu_lost_fn lost = changed && had_frame ? g_lost : NULL;
  void *ctx = g_ctx;
  g_expected_pid = expected_pid;
  g_expected_uid = expected_uid;
  if (changed) {
    memset(g_last_generation, 0, sizeof g_last_generation);
    memset(g_has_frame, 0, sizeof g_has_frame);
  }
  pthread_mutex_unlock(&g_lock);
  if (lost) lost(ctx);
  pthread_mutex_unlock(&g_delivery_lock);
  return 0;
}

void session_gpu_host_stop(void) {
  pthread_mutex_lock(&g_delivery_lock);
  pthread_mutex_lock(&g_lock);
  int was_running = g_running;
  g_running = 0;
  pthread_mutex_unlock(&g_lock);
  pthread_mutex_unlock(&g_delivery_lock);
  if (!was_running) return;
  if (g_thread) pthread_join(g_thread, NULL);
  /* Revoke this receive right. launchd's bootstrap name remains unusable until
   * the boot namespace ends; callers should not restart within one epoch. */
  mach_port_deallocate(mach_task_self(), g_recv_port);
  mach_port_mod_refs(mach_task_self(), g_recv_port, MACH_PORT_RIGHT_RECEIVE, -1);
  g_recv_port = MACH_PORT_NULL;
}

int session_gpu_child_present(pid_t owner_pid, session_gpu_epoch epoch,
                              IOSurfaceRef surface, uint64_t generation,
                              uint32_t crtc, uint32_t width, uint32_t height,
                              uint32_t format) {
  if (!surface || !generation ||
      !valid_geometry(crtc, width, height, format)) return -EINVAL;
  char name[BOOTSTRAP_MAX_NAME_LEN];
  int rc = service_name(name, sizeof name, owner_pid, epoch);
  if (rc) return rc;
  mach_port_t bp = MACH_PORT_NULL, target = MACH_PORT_NULL;
  if (task_get_bootstrap_port(mach_task_self(), &bp) != KERN_SUCCESS ||
      bootstrap_look_up(bp, name, &target) != BOOTSTRAP_SUCCESS) return -ENODEV;
  mach_port_t right = IOSurfaceCreateMachPort(surface);
  if (right == MACH_PORT_NULL) {
    mach_port_deallocate(mach_task_self(), target);
    return -EIO;
  }
  gpu_frame_message m = {0};
  m.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) |
                       MACH_MSGH_BITS_COMPLEX;
  m.header.msgh_size = sizeof m;
  m.header.msgh_remote_port = target;
  m.header.msgh_id = GPU_FRAME_MSG_ID;
  m.body.msgh_descriptor_count = 1;
  m.port.name = right;
  m.port.disposition = MACH_MSG_TYPE_COPY_SEND;
  m.port.type = MACH_MSG_PORT_DESCRIPTOR;
  m.magic = GPU_FRAME_MAGIC;
  m.version = GPU_FRAME_VERSION;
  m.epoch_high = epoch.high;
  m.epoch_low = epoch.low;
  m.generation = generation;
  m.claimed_pid = (uint32_t)getpid();
  m.crtc = crtc;
  m.width = width;
  m.height = height;
  m.format = format;
  kern_return_t kr = mach_msg(&m.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT,
                              sizeof m, 0, MACH_PORT_NULL, 200,
                              MACH_PORT_NULL);
  mach_port_deallocate(mach_task_self(), right);
  mach_port_deallocate(mach_task_self(), target);
  return kr == KERN_SUCCESS ? 0 : (kr == MACH_SEND_TIMED_OUT ? -ETIMEDOUT : -EIO);
}
