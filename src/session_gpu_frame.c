/* Completed IOSurface frame transport: compositor host process -> persistent
 * GUI owner. No pixel mapping or CPU copy occurs on this path. */
#include "session_gpu_frame.h"

#include <bsm/libbsm.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdatomic.h>
#include <servers/bootstrap.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/fileport.h>
#include <sys/stat.h>
#include <unistd.h>

#define GPU_FRAME_MAGIC 0x53474631u
#define GPU_FRAME_MSG_ID 0x53474631
#define GPU_MASTER_MSG_ID 0x53474632
#define GPU_MASTER_RELEASE_MSG_ID 0x53474633
#define GPU_FRAME_VERSION 1u
#define GPU_FRAME_MAX_SIDE 16384u
#define GPU_FRAME_BGRA 0x42475241u
#define GPU_FRAME_POLL_MS 200u

typedef struct {
  mach_msg_header_t header;
  mach_msg_body_t body;
  mach_msg_port_descriptor_t port;
  mach_msg_port_descriptor_t master_port;
  uint32_t magic, version;
  uint64_t epoch_high, epoch_low, sequence, generation;
  uint32_t claimed_pid, crtc, width, height, format;
} gpu_frame_message;

typedef struct {
  mach_msg_header_t header;
  mach_msg_body_t body;
  mach_msg_port_descriptor_t port;
  uint32_t magic, version;
  uint64_t epoch_high, epoch_low;
  uint32_t claimed_pid;
} gpu_master_message;

typedef struct {
  mach_msg_header_t header;
  int32_t result;
} gpu_master_reply;

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
static _Atomic uint64_t g_child_sequence[2];
static int g_has_frame[2];
static session_gpu_present_fn g_present;
static session_gpu_lost_fn g_lost;
static void *g_ctx;
static int g_require_master_proof;
static char g_master_path[1024];
static dev_t g_master_dev;
static ino_t g_master_ino;
static int verify_master_fileport(mach_port_t port);

static int audit_sender(mach_msg_header_t *hdr, size_t cap,
                        pid_t *pid, uid_t *uid) {
  if (hdr->msgh_size + sizeof(mach_msg_audit_trailer_t) > cap) return -EACCES;
  mach_msg_audit_trailer_t *trailer = (mach_msg_audit_trailer_t *)
      ((uint8_t *)hdr + round_msg(hdr->msgh_size));
  if (trailer->msgh_trailer_type != MACH_MSG_TRAILER_FORMAT_0 ||
      trailer->msgh_trailer_size < sizeof(*trailer)) return -EACCES;
  *pid = audit_token_to_pid(trailer->msgh_audit);
  *uid = audit_token_to_euid(trailer->msgh_audit);
  return 0;
}

static int service_name(char *out, size_t cap, pid_t owner, session_gpu_epoch epoch) {
  if (owner <= 1 || !epoch.high || !epoch.low) return -EINVAL;
  int n = snprintf(out, cap, "com.rdx.sessiongpu.%d.%016llx%016llx",
                   owner, (unsigned long long)epoch.high,
                   (unsigned long long)epoch.low);
  return n > 0 && (size_t)n < cap ? 0 : -ENAMETOOLONG;
}

int session_gpu_epoch_parse(const char *text, session_gpu_epoch *out) {
  if (!text || !out || strlen(text) != 32) return -EINVAL;
  char half[17];
  char *end = NULL;
  memcpy(half, text, 16); half[16] = 0;
  errno = 0;
  uint64_t high = strtoull(half, &end, 16);
  if (errno || end != half + 16) return -EINVAL;
  memcpy(half, text + 16, 16); half[16] = 0;
  errno = 0;
  uint64_t low = strtoull(half, &end, 16);
  if (errno || end != half + 16 || !high || !low) return -EINVAL;
  *out = (session_gpu_epoch){high, low};
  return 0;
}

int session_gpu_epoch_format(session_gpu_epoch epoch, char out[33]) {
  if (!out || !epoch.high || !epoch.low) return -EINVAL;
  snprintf(out, 33, "%016llx%016llx", (unsigned long long)epoch.high,
           (unsigned long long)epoch.low);
  return 0;
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
  if ((m->body.msgh_descriptor_count != 1 &&
       m->body.msgh_descriptor_count != 2) ||
      m->port.type != MACH_MSG_PORT_DESCRIPTOR ||
      m->port.name == MACH_PORT_NULL ||
      m->magic != GPU_FRAME_MAGIC || m->version != GPU_FRAME_VERSION ||
      !valid_geometry(m->crtc, m->width, m->height, m->format) ||
      !m->generation || !m->sequence) return -EBADMSG;

  pid_t sender;
  uid_t sender_uid;
  if (audit_sender(hdr, cap, &sender, &sender_uid)) return -EACCES;

  pthread_mutex_lock(&g_lock);
  int master_mode = g_require_master_proof;
  int authorized = g_running &&
      (master_mode || (g_expected_pid > 1 && sender == g_expected_pid)) &&
      sender_uid == g_expected_uid &&
      m->claimed_pid == (uint32_t)sender &&
      m->epoch_high == g_epoch.high && m->epoch_low == g_epoch.low;
  int slot = (int)m->crtc - 1;
  if (authorized && !master_mode &&
      m->sequence <= g_last_generation[slot]) authorized = 0;
  pthread_mutex_unlock(&g_lock);
  if (!authorized) return -EACCES;
  if (master_mode) {
    if (m->body.msgh_descriptor_count != 2 ||
        m->master_port.type != MACH_MSG_PORT_DESCRIPTOR ||
        m->master_port.name == MACH_PORT_NULL ||
        verify_master_fileport(m->master_port.name)) return -EACCES;
  } else if (m->body.msgh_descriptor_count != 1) return -EACCES;

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
  authorized = g_running && (!g_require_master_proof || master_mode) &&
      (master_mode || sender == g_expected_pid) &&
      sender_uid == g_expected_uid &&
      m->epoch_high == g_epoch.high && m->epoch_low == g_epoch.low &&
      ((master_mode && sender != g_expected_pid) ||
       m->sequence > g_last_generation[slot]);
  int replaced = authorized && master_mode && sender != g_expected_pid &&
      (g_has_frame[0] || g_has_frame[1]);
  if (authorized && master_mode && sender != g_expected_pid) {
    g_expected_pid = sender;
    memset(g_last_generation, 0, sizeof g_last_generation);
    memset(g_has_frame, 0, sizeof g_has_frame);
  }
  session_gpu_present_fn present = authorized ? g_present : NULL;
  session_gpu_lost_fn lost = replaced ? g_lost : NULL;
  void *ctx = g_ctx;
  pthread_mutex_unlock(&g_lock);
  if (lost) lost(ctx);
  int rc = present ? (int)present(surface, m->crtc, m->generation, ctx) : -EACCES;
  if (rc == 0) {
    pthread_mutex_lock(&g_lock);
    g_last_generation[slot] = m->sequence;
    g_has_frame[slot] = 1;
    pthread_mutex_unlock(&g_lock);
  }
  pthread_mutex_unlock(&g_delivery_lock);
  CFRelease(surface);
  return rc;
}

static int verify_master_fileport(mach_port_t port) {
  int transferred = fileport_makefd(port);
  if (transferred < 0) return -EBADF;
  struct stat transferred_st, path_st;
  int probe = open(g_master_path, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
  int rc = -EACCES;
  if (probe < 0 || fstat(probe, &path_st) || fstat(transferred, &transferred_st) ||
      !S_ISREG(path_st.st_mode) || !S_ISREG(transferred_st.st_mode) ||
      path_st.st_dev != g_master_dev || path_st.st_ino != g_master_ino ||
      transferred_st.st_dev != g_master_dev || transferred_st.st_ino != g_master_ino)
    goto out;

  /* An independent description must be excluded, while the transferred
   * description must reacquire the same flock. This proves possession of the
   * current DRM-master capability, not merely access to its pathname/inode. */
  if (flock(probe, LOCK_EX | LOCK_NB) == 0) {
    flock(probe, LOCK_UN);
    goto out;
  }
  if (errno != EWOULDBLOCK && errno != EAGAIN) goto out;
  if (flock(transferred, LOCK_EX | LOCK_NB) != 0) goto out;
  rc = 0;
out:
  if (probe >= 0) close(probe);
  close(transferred);
  return rc;
}

static int receive_master(mach_msg_header_t *hdr, size_t cap) {
  if (hdr->msgh_size != sizeof(gpu_master_message) ||
      (hdr->msgh_id != GPU_MASTER_MSG_ID &&
       hdr->msgh_id != GPU_MASTER_RELEASE_MSG_ID) ||
      !(hdr->msgh_bits & MACH_MSGH_BITS_COMPLEX)) return -EBADMSG;
  gpu_master_message *m = (gpu_master_message *)hdr;
  if (m->body.msgh_descriptor_count != 1 ||
      m->port.type != MACH_MSG_PORT_DESCRIPTOR ||
      m->port.name == MACH_PORT_NULL ||
      m->magic != GPU_FRAME_MAGIC || m->version != GPU_FRAME_VERSION)
    return -EBADMSG;
  pid_t sender;
  uid_t sender_uid;
  if (audit_sender(hdr, cap, &sender, &sender_uid)) return -EACCES;

  pthread_mutex_lock(&g_lock);
  int authorized = g_running && g_require_master_proof &&
      sender > 1 && sender_uid == g_expected_uid &&
      m->claimed_pid == (uint32_t)sender &&
      m->epoch_high == g_epoch.high && m->epoch_low == g_epoch.low;
  pthread_mutex_unlock(&g_lock);
  if (!authorized) return -EACCES;
  int rc = verify_master_fileport(m->port.name);
  if (rc) return rc;

  pthread_mutex_lock(&g_delivery_lock);
  pthread_mutex_lock(&g_lock);
  authorized = g_running && g_require_master_proof &&
      sender_uid == g_expected_uid &&
      m->epoch_high == g_epoch.high && m->epoch_low == g_epoch.low;
  int release = hdr->msgh_id == GPU_MASTER_RELEASE_MSG_ID;
  if (release && g_expected_pid != sender) authorized = 0;
  int replaced = authorized && (release || g_expected_pid != sender) &&
      (g_has_frame[0] || g_has_frame[1]);
  if (authorized) {
    g_expected_pid = release ? 0 : sender;
    memset(g_last_generation, 0, sizeof g_last_generation);
    memset(g_has_frame, 0, sizeof g_has_frame);
  }
  session_gpu_lost_fn lost = replaced ? g_lost : NULL;
  void *ctx = g_ctx;
  pthread_mutex_unlock(&g_lock);
  if (lost) lost(ctx);
  pthread_mutex_unlock(&g_delivery_lock);
  return authorized ? 0 : -EACCES;
}

static void reply_result(mach_msg_header_t *request, int32_t id, int result) {
  if (request->msgh_remote_port == MACH_PORT_NULL) return;
  gpu_master_reply reply = {0};
  reply.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
  reply.header.msgh_size = sizeof reply;
  reply.header.msgh_remote_port = request->msgh_remote_port;
  reply.header.msgh_id = id;
  reply.result = result;
  kern_return_t kr = mach_msg(&reply.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT,
                              sizeof reply, 0, MACH_PORT_NULL, 200, MACH_PORT_NULL);
  if (kr == KERN_SUCCESS)
    request->msgh_remote_port = MACH_PORT_NULL;
  else fprintf(stderr, "session_gpu: master reply send failed kr=%d result=%d\n", kr, result);
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
      if (hdr->msgh_id == GPU_MASTER_MSG_ID ||
          hdr->msgh_id == GPU_MASTER_RELEASE_MSG_ID) {
        int rc = receive_master(hdr, sizeof buffer);
        reply_result(hdr, hdr->msgh_id, rc);
      } else if (hdr->msgh_id == GPU_FRAME_MSG_ID) {
        int rc = receive_one(hdr, sizeof buffer);
        reply_result(hdr, GPU_FRAME_MSG_ID, rc);
      }
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

static int host_start_common(pid_t expected_pid, uid_t expected_uid,
                             session_gpu_epoch epoch, const char *master_path,
                             session_gpu_present_fn present,
                             session_gpu_lost_fn lost, void *ctx) {
  if ((!master_path && expected_pid <= 1) || !present || getpid() <= 1 ||
      expected_uid != geteuid()) return -EINVAL;
  struct stat master_st = {0};
  if (master_path) {
    if (!*master_path || strlen(master_path) >= sizeof g_master_path)
      return -ENAMETOOLONG;
    int fd = open(master_path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -errno;
    int invalid = fstat(fd, &master_st) || !S_ISREG(master_st.st_mode) ||
        master_st.st_uid != geteuid() || (master_st.st_mode & 0777) != 0600 ||
        master_st.st_nlink != 1;
    close(fd);
    if (invalid) return -EACCES;
  }
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
  g_require_master_proof = master_path != NULL;
  if (master_path) {
    snprintf(g_master_path, sizeof g_master_path, "%s", master_path);
    g_master_dev = master_st.st_dev;
    g_master_ino = master_st.st_ino;
  } else g_master_path[0] = 0;
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

int session_gpu_host_start(pid_t expected_pid, uid_t expected_uid,
                           session_gpu_epoch epoch,
                           session_gpu_present_fn present,
                           session_gpu_lost_fn lost, void *ctx) {
  return host_start_common(expected_pid, expected_uid, epoch, NULL,
                           present, lost, ctx);
}

int session_gpu_host_start_from_master(uid_t expected_uid,
                                       session_gpu_epoch epoch,
                                       const char *lock_path,
                                       session_gpu_present_fn present,
                                       session_gpu_lost_fn lost, void *ctx) {
  if (!lock_path) return -EINVAL;
  return host_start_common(0, expected_uid, epoch, lock_path,
                           present, lost, ctx);
}

int session_gpu_host_set_producer(pid_t expected_pid, uid_t expected_uid) {
  if (expected_pid <= 1) return -EINVAL;
  pthread_mutex_lock(&g_delivery_lock);
  pthread_mutex_lock(&g_lock);
  if (!g_running || g_require_master_proof) {
    pthread_mutex_unlock(&g_lock);
    pthread_mutex_unlock(&g_delivery_lock);
    return g_running ? -EPERM : -ENODEV;
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

static int child_master_action(pid_t owner_pid, session_gpu_epoch epoch,
                               int master_lock_fd, int32_t message_id) {
  if (master_lock_fd < 0) return -EBADF;
  char name[BOOTSTRAP_MAX_NAME_LEN];
  int rc = service_name(name, sizeof name, owner_pid, epoch);
  if (rc) return rc;
  mach_port_t bp = MACH_PORT_NULL, target = MACH_PORT_NULL;
  if (task_get_bootstrap_port(mach_task_self(), &bp) != KERN_SUCCESS ||
      bootstrap_look_up(bp, name, &target) != BOOTSTRAP_SUCCESS) return -ENODEV;
  fileport_t capability = FILEPORT_NULL;
  if (fileport_makeport(master_lock_fd, &capability) != 0) {
    mach_port_deallocate(mach_task_self(), target);
    return -EBADF;
  }
  mach_port_t reply_port = MACH_PORT_NULL;
  if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
                         &reply_port) != KERN_SUCCESS) {
    mach_port_deallocate(mach_task_self(), capability);
    mach_port_deallocate(mach_task_self(), target);
    return -EIO;
  }
  gpu_master_message m = {0};
  m.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
                                      MACH_MSG_TYPE_MAKE_SEND_ONCE) |
                       MACH_MSGH_BITS_COMPLEX;
  m.header.msgh_size = sizeof m;
  m.header.msgh_remote_port = target;
  m.header.msgh_local_port = reply_port;
  m.header.msgh_id = message_id;
  m.body.msgh_descriptor_count = 1;
  m.port.name = capability;
  m.port.disposition = MACH_MSG_TYPE_COPY_SEND;
  m.port.type = MACH_MSG_PORT_DESCRIPTOR;
  m.magic = GPU_FRAME_MAGIC;
  m.version = GPU_FRAME_VERSION;
  m.epoch_high = epoch.high;
  m.epoch_low = epoch.low;
  m.claimed_pid = (uint32_t)getpid();
  kern_return_t kr = mach_msg(&m.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT,
                              sizeof m, 0, MACH_PORT_NULL, 200, MACH_PORT_NULL);
  mach_port_deallocate(mach_task_self(), capability);
  mach_port_deallocate(mach_task_self(), target);
  if (kr != KERN_SUCCESS) {
    fprintf(stderr, "session_gpu: master request send failed kr=%d\n", kr);
    mach_port_mod_refs(mach_task_self(), reply_port, MACH_PORT_RIGHT_RECEIVE, -1);
    return kr == MACH_SEND_TIMED_OUT ? -ETIMEDOUT : -EIO;
  }
  _Alignas(8) uint8_t reply_buffer[sizeof(gpu_master_reply) + 64] = {0};
  gpu_master_reply *reply = (gpu_master_reply *)reply_buffer;
  kr = mach_msg(&reply->header, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                0, sizeof reply_buffer, reply_port, 500, MACH_PORT_NULL);
  mach_port_mod_refs(mach_task_self(), reply_port, MACH_PORT_RIGHT_RECEIVE, -1);
  if (kr != KERN_SUCCESS) {
    fprintf(stderr, "session_gpu: master reply receive failed kr=%d\n", kr);
    return kr == MACH_RCV_TIMED_OUT ? -ETIMEDOUT : -EIO;
  }
  if (reply->header.msgh_id != message_id ||
      reply->header.msgh_size != sizeof(*reply)) {
    fprintf(stderr, "session_gpu: master reply shape id=%d size=%u expected=%zu\n",
            reply->header.msgh_id, reply->header.msgh_size, sizeof(*reply));
    return -EBADMSG;
  }
  return reply->result;
}

int session_gpu_child_register_master(pid_t owner_pid, session_gpu_epoch epoch,
                                      int master_lock_fd) {
  return child_master_action(owner_pid, epoch, master_lock_fd,
                             GPU_MASTER_MSG_ID);
}

int session_gpu_child_unregister_master(pid_t owner_pid, session_gpu_epoch epoch,
                                        int master_lock_fd) {
  return child_master_action(owner_pid, epoch, master_lock_fd,
                             GPU_MASTER_RELEASE_MSG_ID);
}

static int child_present_common(pid_t owner_pid, session_gpu_epoch epoch,
                                int master_lock_fd, IOSurfaceRef surface,
                                uint64_t generation, uint32_t crtc,
                                uint32_t width, uint32_t height,
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
  fileport_t capability = FILEPORT_NULL;
  if (master_lock_fd >= 0 && fileport_makeport(master_lock_fd, &capability) != 0) {
    mach_port_deallocate(mach_task_self(), right);
    mach_port_deallocate(mach_task_self(), target);
    return -EBADF;
  }
  mach_port_t reply_port = MACH_PORT_NULL;
  if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
                         &reply_port) != KERN_SUCCESS) {
    mach_port_deallocate(mach_task_self(), right);
    if (capability != FILEPORT_NULL) mach_port_deallocate(mach_task_self(), capability);
    mach_port_deallocate(mach_task_self(), target);
    return -EIO;
  }
  gpu_frame_message m = {0};
  m.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
                                      MACH_MSG_TYPE_MAKE_SEND_ONCE) |
                       MACH_MSGH_BITS_COMPLEX;
  m.header.msgh_size = sizeof m;
  m.header.msgh_remote_port = target;
  m.header.msgh_local_port = reply_port;
  m.header.msgh_id = GPU_FRAME_MSG_ID;
  m.body.msgh_descriptor_count = capability == FILEPORT_NULL ? 1 : 2;
  m.port.name = right;
  m.port.disposition = MACH_MSG_TYPE_COPY_SEND;
  m.port.type = MACH_MSG_PORT_DESCRIPTOR;
  if (capability != FILEPORT_NULL) {
    m.master_port.name = capability;
    m.master_port.disposition = MACH_MSG_TYPE_COPY_SEND;
    m.master_port.type = MACH_MSG_PORT_DESCRIPTOR;
  }
  m.magic = GPU_FRAME_MAGIC;
  m.version = GPU_FRAME_VERSION;
  m.epoch_high = epoch.high;
  m.epoch_low = epoch.low;
  m.sequence = master_lock_fd >= 0 ?
      atomic_fetch_add(&g_child_sequence[crtc - 1], 1) + 1 : generation;
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
  if (capability != FILEPORT_NULL) mach_port_deallocate(mach_task_self(), capability);
  mach_port_deallocate(mach_task_self(), target);
  if (kr != KERN_SUCCESS) {
    mach_port_mod_refs(mach_task_self(), reply_port, MACH_PORT_RIGHT_RECEIVE, -1);
    return kr == MACH_SEND_TIMED_OUT ? -ETIMEDOUT : -EIO;
  }
  _Alignas(8) uint8_t reply_buffer[sizeof(gpu_master_reply) + 64] = {0};
  gpu_master_reply *reply = (gpu_master_reply *)reply_buffer;
  kr = mach_msg(&reply->header, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                0, sizeof reply_buffer, reply_port, 2500, MACH_PORT_NULL);
  mach_port_mod_refs(mach_task_self(), reply_port, MACH_PORT_RIGHT_RECEIVE, -1);
  if (kr != KERN_SUCCESS) return kr == MACH_RCV_TIMED_OUT ? -ETIMEDOUT : -EIO;
  if (reply->header.msgh_id != GPU_FRAME_MSG_ID ||
      reply->header.msgh_size != sizeof(*reply)) return -EBADMSG;
  return reply->result;
}

int session_gpu_child_present(pid_t owner_pid, session_gpu_epoch epoch,
                              IOSurfaceRef surface, uint64_t generation,
                              uint32_t crtc, uint32_t width, uint32_t height,
                              uint32_t format) {
  return child_present_common(owner_pid, epoch, -1, surface, generation,
                              crtc, width, height, format);
}

int session_gpu_child_present_master(pid_t owner_pid, session_gpu_epoch epoch,
                                     int master_lock_fd, IOSurfaceRef surface,
                                     uint64_t generation, uint32_t crtc,
                                     uint32_t width, uint32_t height,
                                     uint32_t format) {
  if (master_lock_fd < 0) return -EBADF;
  return child_present_common(owner_pid, epoch, master_lock_fd, surface,
                              generation, crtc, width, height, format);
}
