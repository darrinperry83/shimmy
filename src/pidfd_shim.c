#include "pidfd_shim.h"
#include <sys/event.h>
#include <sys/wait.h>
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <limits.h>
#include <libproc.h>
#include <sys/proc_info.h>
#include "syscalls.h"

// Linux pidfd_open(pid, flags) -> a fd that signals EPOLLIN when `pid` exits. macOS has no pidfd,
// but kqueue(2) with EVFILT_PROC|NOTE_EXIT watches exactly that, and the kqueue fd is pollable and
// can be added to another kqueue (the shim's epoll emulation) via EVFILT_READ. systemd 261's
// sd-event uses pidfd for every child source and returns -errno (no SIGCHLD fallback) when
// pidfd_open() fails, so returning -ENOSYS froze PID 1 at "Failed to acquire PID reference on
// ourselves". The pidfd registry lets waitid(P_PIDFD, ...) and pidfd_send_signal() recover the pid.
#define MAX_PIDFD 512
static struct {
  int fd; pid_t pid;
  dev_t dev; ino_t ino;
  uint64_t birth_sec, birth_usec;
  char socket_name[sizeof(((struct sockaddr_un*)0)->sun_path)];
  int authenticated_socket;
} slots[MAX_PIDFD];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static pid_t target_listener_pid;
static int target_listener_fd = -1;
static pthread_mutex_t target_init_lock = PTHREAD_MUTEX_INITIALIZER;
static int socket_mode_enabled(void) {
  const char* s = getenv("RDPIDFD_SOCKET");
  return s && *s == '1';
}
static int transfer_mode_enabled(void) {
  const char* s = getenv("RDPIDFD_TRANSFER");
  return s && *s == '1';
}
static int transferable_socket_fd(int fd) {
  struct stat st;
  return transfer_mode_enabled() && fstat(fd, &st) == 0 && S_ISSOCK(st.st_mode);
}

static int birth_of(pid_t pid, uint64_t* sec, uint64_t* usec) {
  struct proc_bsdinfo info = {0};
  if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof info) != sizeof info ||
      info.pbi_pid != (uint32_t)pid || info.pbi_start_tvusec >= 1000000) return -1;
  *sec = info.pbi_start_tvsec; *usec = info.pbi_start_tvusec;
  return 0;
}

static int socket_group(char* out, size_t cap) {
  const char* group = getenv("RDSOCKETGROUP");
  if (!group || !*group || group[0] != '/' || strlen(group) >= cap) return -1;
  struct stat st;
  if (stat(group, &st) < 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
      (st.st_mode & 0077)) return -1;
  strcpy(out, group); return 0;
}

static int same_shim_image(pid_t peer) {
  char ours[PROC_PIDPATHINFO_MAXSIZE], theirs[PROC_PIDPATHINFO_MAXSIZE];
  return proc_pidpath(getpid(), ours, sizeof ours) > 0 &&
         proc_pidpath(peer, theirs, sizeof theirs) > 0 && strcmp(ours, theirs) == 0;
}

static int socket_path(char* out, size_t cap, const char* kind, pid_t pid,
                       uint64_t sec, uint64_t usec, uint32_t nonce) {
  char group[sizeof(((struct sockaddr_un*)0)->sun_path)];
  if (socket_group(group, sizeof group) < 0) return -1;
  int n = snprintf(out, cap, "%s/pfd-%s-%d-%llu-%llu-%08x", group, kind, pid,
                   (unsigned long long)sec, (unsigned long long)usec, nonce);
  return n > 0 && (size_t)n < cap ? 0 : -1;
}

static int bind_listener(const char* path) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  if (strlen(path) >= sizeof addr.sun_path) { close(fd); errno = ENAMETOOLONG; return -1; }
  strcpy(addr.sun_path, path);
  unlink(path); // stale pathname from this same host PID after guest execve
  if (bind(fd, (struct sockaddr*)&addr, sizeof addr) < 0 || listen(fd, 16) < 0) {
    int e = errno; close(fd); errno = e; return -1;
  }
  return fd;
}

static int read_exact(int fd, void* buf, size_t n) {
  char* p = buf;
  while (n) { ssize_t got = read(fd, p, n); if (got <= 0) return -1; p += got; n -= (size_t)got; }
  return 0;
}

static void* target_accept_loop(void* unused) {
  (void)unused;
  for (;;) {
    int s = accept(target_listener_fd, NULL, NULL);
    if (s < 0) { if (errno == EINTR) continue; break; }
    uid_t uid; gid_t gid;
    if (getpeereid(s, &uid, &gid) < 0 || uid != geteuid()) { close(s); continue; }
    int peerpid = 0; socklen_t plen = sizeof peerpid;
    if (getsockopt(s, SOL_LOCAL, LOCAL_PEERPID, &peerpid, &plen) < 0 ||
        peerpid <= 0 || !same_shim_image(peerpid)) {
      close(s); continue;
    }
    // The target itself creates the writer. The peer PID on the read end is
    // then kernel-authenticated as the actual target, even after the opener
    // exits. A received writer FD would retain the opener's peer PID instead.
    uint8_t length = 0; char path[sizeof(((struct sockaddr_un*)0)->sun_path)] = {0};
    if (read_exact(s, &length, 1) < 0 || length < 2 || length >= sizeof path ||
        read_exact(s, path, length) < 0 || path[length-1] != 0) { close(s); continue; }
    char group[sizeof path]; uint64_t sec, usec;
    if (socket_group(group, sizeof group) < 0 || birth_of(getpid(), &sec, &usec) < 0) { close(s); continue; }
    char prefix[sizeof path];
    if (snprintf(prefix, sizeof prefix, "%s/pfd-data-%d-%llu-%llu-", group, getpid(),
                 (unsigned long long)sec, (unsigned long long)usec) >= (int)sizeof prefix ||
        strncmp(path, prefix, strlen(prefix)) != 0) { close(s); continue; }
    unsigned nonce; int used=0;
    if (sscanf(path + strlen(prefix), "%x%n", &nonce, &used) != 1 || used != 8 ||
        path[strlen(prefix) + (size_t)used] != 0) { close(s); continue; }
    int writer = socket(AF_UNIX, SOCK_STREAM, 0);
    if (writer >= 0) {
      struct sockaddr_un addr = {.sun_family=AF_UNIX}; strcpy(addr.sun_path, path);
      if (connect(writer, (struct sockaddr*)&addr, sizeof addr) == 0) {
        writer = shim_fd_reserve(writer);
        // The peer must survive this guest process's execve and close_range.
        if (writer >= 0 && fcntl(writer, F_SETFD, 0) == 0) (void)write(s, "A", 1);
        else if (writer >= 0) { shim_fd_unreserve(writer); close(writer); }
      } else close(writer);
    }
    close(s);
  }
  return NULL;
}

static pid_t socket_pid(int fd) {
  char group[sizeof(((struct sockaddr_un*)0)->sun_path)];
  if (socket_group(group, sizeof group) < 0) return 0;
  struct sockaddr_un addr = {0}; socklen_t len = sizeof addr;
  if (getsockname(fd, (struct sockaddr*)&addr, &len) < 0 || addr.sun_family != AF_UNIX) return 0;
  int peerpid=0; socklen_t plen=sizeof peerpid;
  if (getsockopt(fd,SOL_LOCAL,LOCAL_PEERPID,&peerpid,&plen)<0 || peerpid<=0) return 0;
  char prefix[sizeof addr.sun_path];
  if (snprintf(prefix, sizeof prefix, "%s/pfd-data-", group) >= (int)sizeof prefix) return 0;
  size_t off = strlen(prefix);
  if (strncmp(addr.sun_path, prefix, off) != 0) return 0;
  int pid = 0, used = 0; unsigned nonce; unsigned long long sec, usec;
  if (sscanf(addr.sun_path + off, "%d-%llu-%llu-%x%n", &pid, &sec, &usec, &nonce, &used) != 4 ||
      used <= 0 || addr.sun_path[off + (size_t)used] != 0 || pid <= 0) return 0;
  uint64_t actual_sec, actual_usec;
  if (peerpid != pid) return 0;
  if (birth_of(pid, &actual_sec, &actual_usec) < 0) return 0;
  if (actual_sec != sec || actual_usec != usec) return 0;
  return pid;
}

int pidfd_shim_target_init(void) {
  if (!socket_mode_enabled() && !transfer_mode_enabled()) return 0;
  char group[sizeof(((struct sockaddr_un*)0)->sun_path)];
  if (socket_group(group, sizeof group) < 0) return -1;
  pthread_mutex_lock(&target_init_lock);
  pid_t self = getpid();
  if (target_listener_pid == self) { pthread_mutex_unlock(&target_init_lock); return 0; }
  if (target_listener_fd >= 0) close(target_listener_fd); // post-fork copy only
  target_listener_fd = -1;
  uint64_t sec, usec; char path[sizeof(((struct sockaddr_un*)0)->sun_path)];
  if (birth_of(self, &sec, &usec) < 0 ||
      socket_path(path, sizeof path, "ctl", self, sec, usec, 0) < 0) goto fail;
  int fd = bind_listener(path); if (fd < 0) goto fail;
  target_listener_fd = shim_fd_reserve(fd);
  // Reclaim target-owned writer sockets that survived a guest execve. The
  // guest close_range shim must protect them before entering the new image.
  for (int i = 256; i < 4096; i++) {
    if (i == target_listener_fd || fcntl(i, F_GETFD) < 0) continue;
    struct sockaddr_un peer = {0}; socklen_t n = sizeof peer;
    if (getpeername(i, (struct sockaddr*)&peer, &n) < 0) continue;
    char prefix[sizeof peer.sun_path];
    if (snprintf(prefix, sizeof prefix, "%s/pfd-data-%d-%llu-%llu-", group, self,
                 (unsigned long long)sec, (unsigned long long)usec) >= (int)sizeof prefix) continue;
    if (peer.sun_family == AF_UNIX && strncmp(peer.sun_path, prefix, strlen(prefix)) == 0) {
      int reserved = shim_fd_reserve(i);
      if (reserved >= 0) (void)fcntl(reserved, F_SETFD, 0);
    }
  }
  pthread_t thread;
  if (pthread_create(&thread, NULL, target_accept_loop, NULL) != 0) goto fail;
  pthread_detach(thread); target_listener_pid = self;
  pthread_mutex_unlock(&target_init_lock); return 0;
fail:
  if (target_listener_fd >= 0) { shim_fd_unreserve(target_listener_fd); close(target_listener_fd); target_listener_fd = -1; }
  pthread_mutex_unlock(&target_init_lock); return -1;
}

static long pidfd_open_socket(pid_t pid) {
  // The guest's PID 1 is the Shimmy boot process, not macOS launchd.
  if (pid == 1) {
    const char* root = getenv("RDVT_ROOT_PID");
    char* end = NULL;
    long host = root ? strtol(root, &end, 10) : 0;
    if (!root || end == root || *end || host <= 0 || host > INT_MAX) return -ESRCH;
    pid = (pid_t)host;
  }
  uint64_t sec, usec;
  if (birth_of(pid, &sec, &usec) < 0) { if (getenv("RDPIDFD_SOCKET_TRACE")) fprintf(stderr,"[pfd] birth pid=%d failed\n",pid); return -ESRCH; }
  char ctlpath[sizeof(((struct sockaddr_un*)0)->sun_path)];
  char datapath[sizeof(((struct sockaddr_un*)0)->sun_path)];
  if (socket_path(ctlpath, sizeof ctlpath, "ctl", pid, sec, usec, 0) < 0 ||
      socket_path(datapath, sizeof datapath, "data", pid, sec, usec, arc4random()) < 0)
    return -ENAMETOOLONG;
  int listener = bind_listener(datapath);
  if (listener < 0) { if (getenv("RDPIDFD_SOCKET_TRACE")) fprintf(stderr,"[pfd] bind pid=%d errno=%d\n",pid,errno); return -errno; }
  int control = socket(AF_UNIX, SOCK_STREAM, 0);
  if (control < 0) { int e=errno; close(listener); unlink(datapath); return -e; }
  struct sockaddr_un addr = {.sun_family=AF_UNIX};
  strcpy(addr.sun_path, ctlpath);
  int connected = 0;
  for (int tries=0; tries<100; tries++) {
    if (connect(control, (struct sockaddr*)&addr, sizeof addr) == 0) { connected=1; break; }
    if (errno != ENOENT && errno != ECONNREFUSED) break;
    usleep(10000); // target may not have reached shim startup after spawn
  }
  if (!connected) { int e=errno; if (getenv("RDPIDFD_SOCKET_TRACE")) fprintf(stderr,"[pfd] connect pid=%d errno=%d\n",pid,e); close(control); close(listener); unlink(datapath); return -e; }
  uint8_t length=(uint8_t)(strlen(datapath)+1);
  if (write(control,&length,1)!=1 || write(control,datapath,length)!=length) {
    int e=errno ? errno : EIO; close(control); close(listener); unlink(datapath); return -e;
  }
  int reader=accept(listener,NULL,NULL);
  int e=errno; close(listener); unlink(datapath);
  if (reader<0) { if (getenv("RDPIDFD_SOCKET_TRACE")) fprintf(stderr,"[pfd] accept pid=%d errno=%d\n",pid,e); close(control); return -e; }
  int peerpid=0; socklen_t plen=sizeof peerpid;
  char byte;
  if (getsockopt(reader,SOL_LOCAL,LOCAL_PEERPID,&peerpid,&plen)<0 || peerpid!=pid ||
      read(control,&byte,1)!=1 || byte!='A') {
    if (getenv("RDPIDFD_SOCKET_TRACE")) fprintf(stderr,"[pfd] verify pid=%d peer=%d errno=%d\n",pid,peerpid,errno);
    close(control); close(reader); return -EIO;
  }
  close(control);
  (void)fcntl(reader,F_SETFD,FD_CLOEXEC);
  return reader;
}

int pidfd_shim_transferable(int fd) {
  /* A socket pidfd can already be sent. A kqueue cannot: Darwin sendmsg()
   * rejects SCM_RIGHTS for kqueue descriptors with EINVAL. Check the host fd
   * type before consulting the numeric registry, which deliberately retains
   * closed pidfds and may contain a reused ordinary fd number. */
  pid_t pid = socket_pid(fd);
  if (pid > 0) {
    int copy = fcntl(fd, F_DUPFD_CLOEXEC, 256);
    return copy < 0 ? -errno : copy;
  }
  if (kevent(fd, NULL, 0, NULL, 0, NULL) < 0) return 0;
  pid = pidfd_shim_pid(fd);
  if (pid <= 0) return 0;
  return (int)pidfd_open_socket(pid);
}

long pidfd_open_shim(pid_t pid, unsigned int flags) {
  if (flags != 0) return -22;               // Linux pidfd_open takes no flags
  if (pid <= 0) return -3;                  // -ESRCH
  char group[sizeof(((struct sockaddr_un*)0)->sun_path)];
  int socket_mode = socket_mode_enabled();
  if (socket_mode && socket_group(group, sizeof group) < 0) return -EACCES;
  int kq = socket_mode ? (int)pidfd_open_socket(pid) : kqueue();
  if (kq < 0) return socket_mode ? -linux_errno_from_host(-kq) : -errno;
  // The pidfd is leaked (never closed, see pidfd_shim_closed) so its NUMBER stays unique for the
  // registry. Mark it CLOEXEC so that leak does not spread: a fork+exec'd child (systemd's executor)
  // closes it on exec instead of inheriting a live pidfd for a process it doesn't own. Relocate to
  // >= 256 too: systemd's pack_fds() consolidates the service's listen fds onto 3,4,5 and spins
  // forever if a low fd is occupied.
  fcntl(kq, F_SETFD, FD_CLOEXEC);
  if (kq < 256) {
    int hi = fcntl(kq, F_DUPFD_CLOEXEC, 256);
    if (hi >= 0) { close(kq); kq = hi; fcntl(kq, F_SETFD, FD_CLOEXEC); }
  }
  if (!socket_mode) {
    struct kevent kev;
    EV_SET(&kev, (uintptr_t)pid, EVFILT_PROC, EV_ADD | EV_ENABLE, NOTE_EXIT, 0, NULL);
    if (kevent(kq, &kev, 1, NULL, 0, NULL) < 0) { int e = errno; close(kq); return -e; }
  }
  pthread_mutex_lock(&lock);
  int slot = -1;
  for (int i = 0; i < MAX_PIDFD; i++) if (slots[i].fd == kq) { slot = i; break; }  // reuse a recycled fd's slot
  if (slot < 0)
    for (int i = 0; i < MAX_PIDFD; i++) if (slots[i].fd == 0) { slot = i; break; }
  if (slot < 0) {
    if (getenv("RDPIDFDDBG")) {
      FILE* pf = fopen("/tmp/rd_pidfd_dbg.txt", "a");
      if (pf) { fprintf(pf, "pidfd_open REGISTRY FULL (max=%d) pid=%d\n", MAX_PIDFD, pid); fclose(pf); }
    }
    pthread_mutex_unlock(&lock); close(kq); return -24;  // -EMFILE
  }
  slots[slot].fd = kq;
  slots[slot].pid = pid;
  slots[slot].authenticated_socket = 0;
  pthread_mutex_unlock(&lock);
  if (socket_mode) pidfd_shim_register(kq, pid);
  if (getenv("RDPIDFDDBG")) {
    FILE* pf = fopen("/tmp/rd_pidfd_dbg.txt", "a");
    if (pf) { fprintf(pf, "pidfd_open pid=%d -> fd=%d\n", pid, kq); fclose(pf); }
  }
  return kq;
}

void pidfd_shim_closed(int fd) {
  // Deliberately a no-op: systemd's pidref lifecycle closes the pidfd before waitid(P_PIDFD, fd),
  // and if we clear the fd->pid slot here the subsequent waitid sees pid==0 and falls back to
  // waitid(P_ALL) — reaping a RANDOM child instead of the executor, which wedges the unit's state
  // transition ("Starting Coldplug" never finishes). Keep the mapping; pidfd_open_shim() overwrites
  // the slot when the fd number is recycled, which is the only moment the old mapping becomes wrong.
  (void)fd;
}

void pidfd_shim_register(int fd, pid_t pid) {
  if (fd < 0 || pid <= 0) return;
  struct stat st = {0};
  struct sockaddr_un addr = {0}; socklen_t addrlen = sizeof addr;
  uint64_t sec = 0, usec = 0;
  int socket_auth = socket_mode_enabled() ||
                    (transferable_socket_fd(fd) && socket_pid(fd) == pid);
  if (socket_auth && (socket_pid(fd) != pid || fstat(fd, &st) < 0 ||
                      getsockname(fd, (struct sockaddr*)&addr, &addrlen) < 0 ||
                      addr.sun_family != AF_UNIX ||
                      birth_of(pid, &sec, &usec) < 0)) return;
  pthread_mutex_lock(&lock);
  // Reuse an existing slot for this fd if present, else take the next free slot.
  int slot = -1;
  for (int i = 0; i < MAX_PIDFD; i++) if (slots[i].fd == fd) { slot = i; break; }
  if (slot < 0)
    for (int i = 0; i < MAX_PIDFD; i++) if (slots[i].fd == 0) { slot = i; break; }
  if (slot >= 0) {
    slots[slot].fd = fd; slots[slot].pid = pid;
    slots[slot].authenticated_socket = socket_auth;
    if (socket_auth) {
      slots[slot].dev = st.st_dev; slots[slot].ino = st.st_ino;
      slots[slot].birth_sec = sec; slots[slot].birth_usec = usec;
      snprintf(slots[slot].socket_name, sizeof slots[slot].socket_name, "%s", addr.sun_path);
    }
  }
  pthread_mutex_unlock(&lock);
  if (getenv("RDPIDFDDBG")) {
    FILE* pf = fopen("/tmp/rd_pidfd_dbg.txt", "a");
    if (pf) { fprintf(pf, "pidfd_register fd=%d pid=%d\n", fd, pid); fclose(pf); }
  }
}

pid_t pidfd_shim_pid(int fd) {
  // A socket may be received through SCM_RIGHTS, closed, or have its fd number
  // reused. Never trust the numeric registry for this representation: inspect
  // its kernel peer and target birth identity on every lookup.
  if (socket_mode_enabled() || transferable_socket_fd(fd)) {
    pid_t live = socket_pid(fd);
    if (live > 0) { pidfd_shim_register(fd, live); return live; }
    // LOCAL_PEERPID stops working when the target closes its socket on exit.
    // The kernel socket identity and name remain; accept only a prior,
    // authenticated observation of that exact socket, and only while its
    // original PID has not been reused.
    struct stat st; struct sockaddr_un addr = {0}; socklen_t len = sizeof addr;
    if (fstat(fd, &st) < 0 || getsockname(fd, (struct sockaddr*)&addr, &len) < 0 ||
        addr.sun_family != AF_UNIX) return 0;
    pid_t cached = 0; uint64_t sec = 0, usec = 0;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_PIDFD; i++)
      if (slots[i].fd == fd && slots[i].authenticated_socket &&
          slots[i].dev == st.st_dev && slots[i].ino == st.st_ino &&
          strcmp(slots[i].socket_name, addr.sun_path) == 0) {
        cached = slots[i].pid; sec = slots[i].birth_sec; usec = slots[i].birth_usec; break;
      }
    pthread_mutex_unlock(&lock);
    if (cached <= 0) return 0;
    uint64_t now_sec, now_usec;
    if (birth_of(cached, &now_sec, &now_usec) == 0)
      return now_sec == sec && now_usec == usec ? cached : 0;
    return kill(cached, 0) < 0 && errno == ESRCH ? cached : 0;
  }
  pid_t pid = 0;
  pthread_mutex_lock(&lock);
  for (int i = 0; i < MAX_PIDFD; i++) if (slots[i].fd == fd) { pid = slots[i].pid; break; }
  pthread_mutex_unlock(&lock);
  if (!pid) {
    pid = socket_pid(fd);
    if (pid) pidfd_shim_register(fd, pid);
  }
  return pid;
}

int pidfd_shim_waitid(int fd, id_t id, siginfo_t* si, int hopt) {
  pid_t pid = pidfd_shim_pid(fd);
  if (pid <= 0) {
    if (socket_mode_enabled() || transferable_socket_fd(fd)) { errno = EBADF; return -1; }
    // Not a pidfd we issued. The guest may hold a stale/unset pidfd (systemd uses fd=INT_MAX as a
    // "no pidfd" sentinel in some paths). On Linux this is EBADF; emulate "wait for any child"
    // instead so pidfd verification doesn't wedge every executor spawn with EBADF.
    if (getenv("RDPIDFDDBG")) {
      FILE* pf = fopen("/tmp/rd_pidfd_dbg.txt", "a");
      if (pf) { fprintf(pf, "pidfd_waitid fd=%d -> fallback P_ALL\n", fd); fclose(pf); }
    }
    (void)id;
    return waitid(P_ALL, 0, si, hopt);
  }
  (void)id;
  return waitid(P_PID, pid, si, hopt);
}

int pidfd_shim_send_signal(int fd, int sig) {
  pid_t pid = pidfd_shim_pid(fd);
  if (socket_mode_enabled() || transferable_socket_fd(fd)) {
    if (pid <= 0) { errno = EBADF; return -1; }
    // A host kill(pid) can address a reused PID between lookup and delivery.
    // This representation needs a target-owned signaling protocol before it
    // can promise Linux pidfd_send_signal for nonzero signals.
    if (sig != 0) { errno = ENOTSUP; return -1; }
    uint64_t sec, usec;
    if (birth_of(pid, &sec, &usec) < 0) { errno = ESRCH; return -1; }
    return kill(pid, 0);
  }
  if (pid <= 0) {
    // Stale/unset pidfd (systemd's fd=INT_MAX sentinel). A signal-0 existence probe is answered
    // optimistically: the process DOES exist, we just lost the fd->pid mapping.
    if (sig == 0) return 0;
    errno = EBADF; return -1;
  }
  return kill(pid, sig);
}
