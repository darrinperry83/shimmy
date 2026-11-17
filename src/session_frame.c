// Native-boot DRM frame handoff. A Linux compositor is a different host process
// from PID 1's persistent AppKit window. This temporary CPU-compatible carrier
// makes that window the sole presenter; it is not the eventual rdmetal GPU path.
#include "session_frame.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

extern int shim_fd_reserve(int fd);
extern void shim_fd_unreserve(int fd);

#define FRAME_MAGIC 0x52444652u
#define FRAME_MAX_BYTES (64u * 1024u * 1024u)
#define FRAME_MAX_SIDE 8192u

struct frame_shared {
  _Atomic uint64_t seq; // odd while producer writes; even when complete
  uint32_t magic, width, height, crtc;
  uint8_t pixels[];
};

struct frame_notice {
  uint32_t magic, pid, nonce, crtc, width, height;
  uint64_t seq;
};

struct frame_slot {
  int fd;
  struct frame_shared* map;
  char path[sizeof(((struct sockaddr_un*)0)->sun_path)];
  uint32_t pid, nonce;
  uint64_t last_seq;
};

static pthread_mutex_t g_sender_lock = PTHREAD_MUTEX_INITIALIZER;
static struct frame_slot g_sender[2] = {{.fd=-1},{.fd=-1}};
static struct frame_slot g_receiver[2] = {{.fd=-1},{.fd=-1}};
static int g_send_socket = -1;
static pid_t g_send_socket_pid;
static int g_recv_socket = -1;
static pthread_t g_recv_thread;
static _Atomic int g_recv_running;
static session_frame_present_fn g_present;
static void (*g_lost)(void);

static int frame_size(uint32_t w, uint32_t h, size_t* n) {
  if (!w || !h || w > FRAME_MAX_SIDE || h > FRAME_MAX_SIDE) return -1;
  uint64_t bytes = (uint64_t)w * h * 4;
  if (bytes > FRAME_MAX_BYTES) return -1;
  *n = (size_t)bytes;
  return 0;
}

static int root_pid(void) {
  const char* s = getenv("RDVT_ROOT_PID");
  if (!s || !*s) return -1;
  for (const char* p=s; *p; p++) if (*p < '0' || *p > '9') return -1;
  long n = strtol(s, NULL, 10);
  return n > 1 && n <= INT32_MAX ? (int)n : -1;
}

static int frame_path(char* dst, size_t cap, const char* leaf) {
  int pid = root_pid();
  if (pid < 0) return -1;
  int n = snprintf(dst, cap, "/tmp/rdx-%d/%s", pid, leaf);
  return n > 0 && (size_t)n < cap ? 0 : -1;
}

static int namespace_valid(void) {
  char dir[sizeof(((struct sockaddr_un*)0)->sun_path)];
  if (frame_path(dir, sizeof dir, "") != 0) return 0;
  dir[strlen(dir)-1] = 0;
  struct stat st;
  return lstat(dir, &st) == 0 && S_ISDIR(st.st_mode) &&
         st.st_uid == geteuid() && (st.st_mode & 0777) == 0700;
}

static int notice_path(char* dst, size_t cap, uint32_t pid, uint32_t crtc,
                       uint32_t nonce) {
  char leaf[64];
  snprintf(leaf, sizeof leaf, "frame.%u.%u.%08x", pid, crtc, nonce);
  return frame_path(dst, cap, leaf);
}

static void slot_close(struct frame_slot* slot, int remove_file) {
  if (slot->map) munmap(slot->map, sizeof(struct frame_shared) + FRAME_MAX_BYTES);
  if (slot->fd >= 0) { shim_fd_unreserve(slot->fd); close(slot->fd); }
  if (remove_file && slot->path[0]) unlink(slot->path);
  memset(slot, 0, sizeof *slot);
  slot->fd = -1;
}

static int sender_slot_open(struct frame_slot* slot, uint32_t crtc) {
  if (slot->map) return 0;
  if (!namespace_valid()) return -1;
  uint32_t nonce = arc4random();
  char path[sizeof slot->path];
  if (notice_path(path, sizeof path, (uint32_t)getpid(), crtc, nonce)) return -1;
  int fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) return -1;
  size_t total = sizeof(struct frame_shared) + FRAME_MAX_BYTES;
  if (ftruncate(fd, (off_t)total) || fchmod(fd, 0600)) {
    close(fd); unlink(path); return -1;
  }
  struct frame_shared* map = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED) { close(fd); unlink(path); return -1; }
  memset(map, 0, sizeof *map);
  slot->fd = shim_fd_reserve(fd);
  slot->map = map;
  slot->pid = (uint32_t)getpid();
  slot->nonce = nonce;
  snprintf(slot->path, sizeof slot->path, "%s", path);
  return 0;
}

static int send_socket(void) {
  if (g_send_socket >= 0 && g_send_socket_pid == getpid()) return g_send_socket;
  if (g_send_socket >= 0) {
    shim_fd_unreserve(g_send_socket); close(g_send_socket);
  }
  int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (fd < 0) return -1;
  fcntl(fd, F_SETFD, FD_CLOEXEC);
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  g_send_socket = shim_fd_reserve(fd);
  g_send_socket_pid = getpid();
  return g_send_socket;
}

long session_frame_child_present(const uint8_t* bgra, uint32_t width,
                                 uint32_t height, uint32_t crtc) {
  size_t bytes;
  if (!bgra || (crtc != 1 && crtc != 2) || frame_size(width,height,&bytes)) return -EINVAL;
  if (root_pid() < 0 || root_pid() == (int)getpid()) return -EINVAL;
  pthread_mutex_lock(&g_sender_lock);
  struct frame_slot* slot = &g_sender[crtc-1];
  if (slot->pid != (uint32_t)getpid()) slot_close(slot, 0); // forked process, not its parent's slot
  if (sender_slot_open(slot, crtc) || send_socket() < 0) {
    pthread_mutex_unlock(&g_sender_lock); return -ENODEV;
  }
  uint64_t seq = slot->last_seq + 2;
  atomic_store_explicit(&slot->map->seq, seq-1, memory_order_seq_cst);
  atomic_thread_fence(memory_order_seq_cst); // publish "writing" before any pixel mutation
  slot->map->magic = FRAME_MAGIC;
  slot->map->width = width; slot->map->height = height; slot->map->crtc = crtc;
  memcpy(slot->map->pixels, bgra, bytes);
  atomic_store_explicit(&slot->map->seq, seq, memory_order_release);
  slot->last_seq = seq;
  struct frame_notice msg = {FRAME_MAGIC, (uint32_t)getpid(), slot->nonce, crtc,
                             width, height, seq};
  char path[sizeof(((struct sockaddr_un*)0)->sun_path)];
  int rc = frame_path(path, sizeof path, "frame.sock");
  struct sockaddr_un server = {.sun_family=AF_UNIX};
  if (!rc) snprintf(server.sun_path, sizeof server.sun_path, "%s", path);
  ssize_t sent = rc ? -1 : sendto(g_send_socket, &msg, sizeof msg, 0,
                                 (struct sockaddr*)&server, sizeof server);
  if (getenv("RDGPU_PRESENT_STATS")) {
    static unsigned traced;
    if (++traced <= 8 || sent != sizeof msg)
      fprintf(stderr, "[frame-bridge] child pid=%d root=%d crtc=%u seq=%llu %ux%u send=%zd errno=%d\n",
              (int)getpid(), root_pid(), crtc, (unsigned long long)seq,
              width, height, sent, sent < 0 ? errno : 0);
  }
  pthread_mutex_unlock(&g_sender_lock);
  return sent == sizeof msg ? 0 : -ENODEV;
}

static int receiver_slot_map(struct frame_slot* slot, const struct frame_notice* msg) {
  if (slot->map && slot->pid == msg->pid && slot->nonce == msg->nonce) return 0;
  char path[sizeof slot->path];
  if (notice_path(path, sizeof path, msg->pid, msg->crtc, msg->nonce)) return -1;
  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return -1;
  struct stat st;
  size_t total = sizeof(struct frame_shared) + FRAME_MAX_BYTES;
  if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
      (st.st_mode & 0777) != 0600 || st.st_size != (off_t)total || st.st_nlink != 1) {
    close(fd); return -1;
  }
  struct frame_shared* map = mmap(NULL, total, PROT_READ, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED) { close(fd); return -1; }
  slot_close(slot, 1); // remove the previous session's pathname, never its active mapping
  slot->fd = shim_fd_reserve(fd);
  slot->map = map;
  slot->pid = msg->pid; slot->nonce = msg->nonce;
  snprintf(slot->path, sizeof slot->path, "%s", path);
  return 0;
}

static void* receiver_loop(void* unused) {
  (void)unused;
  uint8_t* snapshot = NULL;
  size_t snapshot_cap = 0;
  while (atomic_load(&g_recv_running)) {
    struct pollfd pollfd = {.fd=g_recv_socket,.events=POLLIN};
    int pr = poll(&pollfd, 1, 200);
    if (pr > 0 && (pollfd.revents & POLLIN)) {
      struct frame_notice msg;
      ssize_t n = recv(g_recv_socket, &msg, sizeof msg, 0);
      size_t bytes;
      if (n == sizeof msg && msg.magic == FRAME_MAGIC &&
          (msg.crtc == 1 || msg.crtc == 2) && !frame_size(msg.width,msg.height,&bytes) &&
          msg.pid > 1 && kill((pid_t)msg.pid,0) == 0 && namespace_valid()) {
        struct frame_slot* slot = &g_receiver[msg.crtc-1];
        if (receiver_slot_map(slot, &msg) == 0 && msg.seq > slot->last_seq &&
            (msg.seq & 1) == 0) {
          if (snapshot_cap < bytes) {
            uint8_t* next = realloc(snapshot, bytes);
            if (!next) continue;
            snapshot = next; snapshot_cap = bytes;
          }
          uint64_t before = atomic_load_explicit(&slot->map->seq, memory_order_acquire);
          if (before != msg.seq || slot->map->magic != FRAME_MAGIC ||
              slot->map->width != msg.width || slot->map->height != msg.height ||
              slot->map->crtc != msg.crtc) continue;
          memcpy(snapshot, slot->map->pixels, bytes);
          uint64_t after = atomic_load_explicit(&slot->map->seq, memory_order_acquire);
          if (before != after || after != msg.seq) continue; // producer raced this copy
          slot->last_seq = msg.seq;
          if (g_present) {
            long rc = g_present(snapshot, msg.width, msg.height, msg.crtc);
            if (getenv("RDGPU_PRESENT_STATS")) {
              static unsigned traced;
              if (++traced <= 8 || rc) {
                int nonblack = 0;
                for (size_t p = 0; p + 2 < bytes; p += 4096)
                  if (snapshot[p] || snapshot[p+1] || snapshot[p+2]) { nonblack = 1; break; }
                fprintf(stderr, "[frame-bridge] owner pid=%d child=%u crtc=%u seq=%llu %ux%u nonblack=%d present=%ld\n",
                        (int)getpid(), msg.pid, msg.crtc,
                        (unsigned long long)msg.seq, msg.width, msg.height, nonblack, rc);
              }
            }
          }
        }
      }
    }
    // A dead compositor cannot leave its unlocked last frame on the host
    // window. The callback switches to a text VT; a new greeter may then draw.
    int lost = 0;
    for (int i=0;i<2;i++) {
      struct frame_slot* slot = &g_receiver[i];
      if (slot->pid && kill((pid_t)slot->pid,0) < 0 && errno == ESRCH) {
        slot_close(slot,1); lost = 1;
      }
    }
    if (lost && !g_receiver[0].pid && !g_receiver[1].pid && g_lost) g_lost();
  }
  free(snapshot);
  return NULL;
}

// The lost callback clears the graphical frame when its producing process exits.
int session_frame_host_start_with_lost(session_frame_present_fn present, void (*lost)(void)) {
  if (root_pid() != (int)getpid() || !namespace_valid() || !present) return -1;
  if (atomic_load(&g_recv_running)) return 0;
  char path[sizeof(((struct sockaddr_un*)0)->sun_path)];
  if (frame_path(path, sizeof path, "frame.sock")) return -1;
  int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (fd < 0) return -1;
  fcntl(fd, F_SETFD, FD_CLOEXEC);
  struct sockaddr_un addr = {.sun_family=AF_UNIX};
  snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
  unlink(path);
  if (bind(fd, (struct sockaddr*)&addr, sizeof addr) < 0) { close(fd); return -1; }
  chmod(path,0600);
  g_recv_socket = shim_fd_reserve(fd);
  g_present = present; g_lost = lost;
  atomic_store(&g_recv_running,1);
  if (pthread_create(&g_recv_thread,NULL,receiver_loop,NULL) != 0) {
    atomic_store(&g_recv_running,0);
    shim_fd_unreserve(g_recv_socket); close(g_recv_socket); g_recv_socket=-1;
    unlink(path); return -1;
  }
  return 0;
}

int session_frame_host_start(session_frame_present_fn present) {
  return session_frame_host_start_with_lost(present,NULL);
}

void session_frame_host_stop(void) {
  if (!atomic_exchange(&g_recv_running,0)) return;
  pthread_join(g_recv_thread,NULL);
  shim_fd_unreserve(g_recv_socket); close(g_recv_socket); g_recv_socket=-1;
  for (int i=0;i<2;i++) slot_close(&g_receiver[i],1);
  char path[sizeof(((struct sockaddr_un*)0)->sun_path)];
  if (frame_path(path,sizeof path,"frame.sock") == 0) unlink(path);
}
