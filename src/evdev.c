// evdev.c — synthetic /dev/input/event0 (keyboard) + /dev/input/event1 (pointer).
//
// WHY. Retiring the nested-Wayland compositor removed the outer wl_seat, so native-DRM
// Hyprland has no input source: its aquamarine DRM backend opens libinput on the DRM
// master's seat, and libinput enumerates /dev/input via udev. This file answers that:
// it serves two evdev nodes whose struct input_event records are fed from window.m's
// NSEvent capture (the SAME queue that used to feed wlsrv's wl_seat), plus the EVIOCG*
// ioctls and the /sys/class/input + /run/udev/data surface libinput's udev backend reads.
//
// Each node is a pipe: open() hands the guest a dup of the read end (so read()/poll()/
// epoll() behave like a real evdev fd, exactly as drmasahi.c does for the DRM event pipe),
// and the input pump writes 24-byte input_event records into the write end.
#include "evdev.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>

// ---- evdev constants (linux/input.h, linux/input-event-codes.h) -------------------------
#define EV_SYN         0x00
#define EV_KEY         0x01
#define EV_REL         0x02
#define EV_ABS         0x03
#define EV_MSC         0x04
#define EV_REP         0x14
#define SYN_REPORT     0
#define REL_X          0x00
#define REL_Y          0x01
#define REL_HWHEEL     0x06
#define REL_WHEEL      0x08
#define BTN_LEFT       0x110
#define BTN_RIGHT      0x111
#define BTN_MIDDLE     0x112
#define BUS_VIRTUAL    0x06
#define INPUT_MAJOR    13
#define EVDEV_EVENT0_MINOR 64
#define EVDEV_EVENT1_MINOR 65

// Linux struct input_event (64-bit): timeval{long,long} + type + code + value = 24 bytes.
// Deliberately NOT the host struct timeval (macOS tv_usec is 32-bit).
struct linux_input_event {
  int64_t  tv_sec;
  int64_t  tv_usec;
  uint16_t type;
  uint16_t code;
  int32_t  value;
};
_Static_assert(sizeof(struct linux_input_event) == 24, "linux_input_event");

struct linux_input_id {
  uint16_t bustype, vendor, product, version;
};
struct linux_input_absinfo {
  int32_t value, minimum, maximum, fuzz, flat, resolution;
};
_Static_assert(sizeof(struct linux_input_id) == 8, "linux_input_id");
_Static_assert(sizeof(struct linux_input_absinfo) == 24, "linux_input_absinfo");

// ---- ioctl decode (same shape as drmasahi.c) --------------------------------------------
#define IOC_NRSHIFT    0
#define IOC_TYPESHIFT  8
#define IOC_SIZESHIFT  16
#define IOC_DIRSHIFT   30
#define IOC_NR(x)   (((x) >> IOC_NRSHIFT)   & 0xFF)
#define IOC_TYPE(x) (((x) >> IOC_TYPESHIFT) & 0xFF)
#define IOC_SIZE(x) (((x) >> IOC_SIZESHIFT) & 0x3FFF)
#define IOC_DIR(x)  (((x) >> IOC_DIRSHIFT)  & 0x3)
#define EVDEV_TYPE   'E'

// EVIOCG* request numbers (linux/input.h). EVIOCGBIT's nr is 0x20 + ev type.
#define EVIOC_NR_ID        0x02
#define EVIOC_NR_REP       0x03
#define EVIOC_NR_NAME      0x06
#define EVIOC_NR_PHYS      0x07
#define EVIOC_NR_UNIQ      0x08
#define EVIOC_NR_PROP      0x09
#define EVIOC_NR_KEY       0x18
#define EVIOC_NR_LED       0x19
#define EVIOC_NR_SND       0x1a
#define EVIOC_NR_SW        0x1b
#define EVIOC_NR_GBIT_BASE 0x20
#define EVIOC_NR_ABS_BASE  0x40
#define EVIOC_NR_SCLOCKID  0xa0

// ---- per-device state -------------------------------------------------------------------
struct evdev_device {
  int created;
  int pipe_rd;    // canonical read end (guests get dups)
  int pipe_wr;    // pump writes input_event records here
};

static struct evdev_device g_devs[2];

#define EVDEV_MAXFD 64
static int g_fds[EVDEV_MAXFD];    // guest-visible fd
static int g_devf[EVDEV_MAXFD];   // device index for that fd
static int g_nfd;

static int tracing(void) {
  static int t = -1;
  if (t < 0) { const char* e = getenv("RDEVDEV_TRACE"); t = (e && *e && *e != '0'); }
  return t;
}

static int ensure_pipe(int dev) {
  struct evdev_device* d = &g_devs[dev];
  if (d->created) return 0;
  int p[2];
  if (pipe(p) != 0) return -1;
  // Read end non-blocking: libinput reads under epoll and treats EAGAIN as "drained".
  fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL) | O_NONBLOCK);
  // Write end non-blocking: a full pipe (libinput stalled) must drop, not block the pump.
  fcntl(p[1], F_SETFL, fcntl(p[1], F_GETFL) | O_NONBLOCK);
  fcntl(p[0], F_SETFD, FD_CLOEXEC);
  fcntl(p[1], F_SETFD, FD_CLOEXEC);
  d->pipe_rd = p[0];
  d->pipe_wr = p[1];
  d->created = 1;
  return 0;
}

int evdev_is_node(const char* p) {
  if (!p) return 0;
  return strcmp(p, "/dev/input/event0") == 0 || strcmp(p, "/dev/input/event1") == 0;
}

static int dev_index_of_path(const char* p) {
  if (!p) return -1;
  if (strcmp(p, "/dev/input/event0") == 0) return EVDEV_DEV_KBD;
  if (strcmp(p, "/dev/input/event1") == 0) return EVDEV_DEV_PTR;
  return -1;
}

int evdev_open_node(const char* path) {
  int dev = dev_index_of_path(path);
  if (dev < 0) return -1;
  if (ensure_pipe(dev) != 0) return -1;
  int fd = fcntl(g_devs[dev].pipe_rd, F_DUPFD_CLOEXEC, 0);
  if (fd < 0) return -1;
  evdev_register_fd(fd, dev);
  if (tracing()) fprintf(stderr, "[evdev] opened %s as fd %d\n", path, fd);
  return fd;
}

void evdev_register_fd(int fd, int dev) {
  if (fd < 0 || g_nfd >= EVDEV_MAXFD) return;
  g_fds[g_nfd] = fd;
  g_devf[g_nfd] = dev;
  g_nfd++;
}

void evdev_forget_fd(int fd) {
  for (int i = 0; i < g_nfd; i++)
    if (g_fds[i] == fd) { g_fds[i] = g_fds[--g_nfd]; g_devf[i] = g_devf[g_nfd]; return; }
}

int evdev_owns_fd(int fd) {
  for (int i = 0; i < g_nfd; i++) if (g_fds[i] == fd) return 1;
  return 0;
}

int evdev_device_of_fd(int fd) {
  for (int i = 0; i < g_nfd; i++) if (g_fds[i] == fd) return g_devf[i];
  return -1;
}

// ---- capability bitmaps ----------------------------------------------------------------
static void set_bit(unsigned char* bits, int bit) { bits[bit >> 3] |= (unsigned char)(1u << (bit & 7)); }

static const char* evdev_name(int dev) {
  return dev == EVDEV_DEV_KBD ? "rdmetal virtual keyboard" : "rdmetal virtual pointer";
}

// Fill `out` (cap bytes) with the EV_*/KEY/REL bits for a device + ev type.
static void fill_bits(int dev, int ev_type, unsigned char* out, int cap) {
  if (cap <= 0) return;
  memset(out, 0, (size_t)cap);
  if (ev_type == 0) {                                   // EV_* capability bits
    set_bit(out, EV_SYN);
    set_bit(out, EV_KEY);
    if (dev == EVDEV_DEV_KBD) set_bit(out, EV_REP);
    else                     set_bit(out, EV_REL);
    return;
  }
  if (ev_type == EV_KEY) {                              // KEY_*/BTN_* bits
    if (dev == EVDEV_DEV_KBD) {
      for (int k = 1; k <= 255; k++) set_bit(out, k);   // every keycode we might emit
    } else {
      set_bit(out, BTN_LEFT);
      set_bit(out, BTN_RIGHT);
      set_bit(out, BTN_MIDDLE);
    }
    return;
  }
  if (ev_type == EV_REL && dev == EVDEV_DEV_PTR) {      // REL_* bits
    set_bit(out, REL_X);
    set_bit(out, REL_Y);
    set_bit(out, REL_HWHEEL);
    set_bit(out, REL_WHEEL);
    return;
  }
  // EV_ABS / EV_LED / EV_SND / EV_SW / EV_MSC: empty.
}

static void put_string(void* dst, uint64_t len, const char* s) {
  if (!dst || !len) return;
  memset(dst, 0, (size_t)len);
  memcpy(dst, s, strlen(s) < (size_t)len ? strlen(s) : (size_t)len);
}

// ---- injection -------------------------------------------------------------------------
void evdev_inject(int dev, uint16_t type, uint16_t code, int32_t value) {
  if (dev < 0 || dev > 1 || !g_devs[dev].created) return;
  struct linux_input_event e;
  memset(&e, 0, sizeof e);
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  e.tv_sec  = ts.tv_sec;
  e.tv_usec = ts.tv_nsec / 1000;
  e.type    = type;
  e.code    = code;
  e.value   = value;
  ssize_t r = write(g_devs[dev].pipe_wr, &e, sizeof e);
  (void)r;                                             // EAGAIN (full) or EPIPE (closed): drop
}

void evdev_syn(int dev) { evdev_inject(dev, EV_SYN, SYN_REPORT, 0); }

// ---- ioctl -----------------------------------------------------------------------------
long evdev_ioctl(int fd, unsigned long req, unsigned long arg) {
  if (!evdev_owns_fd(fd)) return -25;                  // -ENOTTY
  int dev = evdev_device_of_fd(fd);
  if (dev < 0) return -25;
  unsigned type = IOC_TYPE(req), nr = IOC_NR(req), sz = IOC_SIZE(req);
  if (tracing())
    fprintf(stderr, "[evdev] fd=%d ioctl type=0x%x nr=0x%02x dir=%u sz=%u\n",
            fd, type, nr, (unsigned)IOC_DIR(req), sz);
  if (type != EVDEV_TYPE) return -25;

  // EVIOCGBIT(ev, len): the capability bitmap for an event type.
  if (nr >= EVIOC_NR_GBIT_BASE && nr < EVIOC_NR_GBIT_BASE + 0x20) {
    int ev_type = (int)(nr - EVIOC_NR_GBIT_BASE);
    unsigned char bits[128];
    fill_bits(dev, ev_type, bits, (int)sizeof bits);
    unsigned n = sz < sizeof bits ? sz : (unsigned)sizeof bits;
    if (arg && n) memcpy((void*)(uintptr_t)arg, bits, n);
    if (tracing()) fprintf(stderr, "[evdev] fd=%d EVIOCGBIT(ev=%d) len=%u -> ok\n", fd, ev_type, sz);
    return 0;
  }

  if (nr == EVIOC_NR_NAME) { put_string((void*)(uintptr_t)arg, sz, evdev_name(dev)); return 0; }
  if (nr == EVIOC_NR_PHYS) {
    const char* phys = dev == EVDEV_DEV_KBD ? "rdmetal/input0" : "rdmetal/input1";
    put_string((void*)(uintptr_t)arg, sz, phys);
    return 0;
  }
  if (nr == EVIOC_NR_UNIQ) { put_string((void*)(uintptr_t)arg, sz, ""); return 0; }

  if (nr == 0x01 && arg) { *(int*)(uintptr_t)arg = 0x010001; return 0; }   // EVIOCGVERSION

  if (nr == EVIOC_NR_ID && arg) {
    struct linux_input_id* id = (struct linux_input_id*)(uintptr_t)arg;
    id->bustype = BUS_VIRTUAL;
    id->vendor  = 0x7264;                              // 'rd'
    id->product = (uint16_t)(dev == EVDEV_DEV_KBD ? 0x0001 : 0x0002);
    id->version = 0x0100;
    return 0;
  }

  if (nr == EVIOC_NR_REP && arg) {                     // EVIOCGREP: delay, period
    unsigned int* rep = (unsigned int*)(uintptr_t)arg;
    rep[0] = 250; rep[1] = 33;
    return 0;
  }

  if (nr == EVIOC_NR_KEY || nr == EVIOC_NR_LED || nr == EVIOC_NR_SND || nr == EVIOC_NR_SW) {
    // Current key/led/snd/switch state: all zero (nothing held).
    if (arg && sz) memset((void*)(uintptr_t)arg, 0, sz);
    return 0;
  }

  if (nr == EVIOC_NR_PROP && arg) {                    // EVIOCGPROP: empty (no input props)
    if (sz) memset((void*)(uintptr_t)arg, 0, sz);
    return 0;
  }

  if (nr >= EVIOC_NR_ABS_BASE && nr < EVIOC_NR_ABS_BASE + 0x40 && arg) {   // EVIOCGABS
    struct linux_input_absinfo* a = (struct linux_input_absinfo*)(uintptr_t)arg;
    memset(a, 0, sizeof *a);
    return 0;
  }

  if (nr == EVIOC_NR_SCLOCKID) {                       // EVIOCSCLOCKID (write): accept
    return 0;
  }

  if (tracing())
    fprintf(stderr, "[evdev] fd=%d ioctl type=%u nr=0x%02x sz=%u -> -EINVAL\n", fd, type, nr, sz);
  return -22;                                          // -EINVAL
}

// ---- stat / sysfs / udev ---------------------------------------------------------------
static int evdev_minor_of_path(const char* path) {
  // event1 (class form) or 13:65 (devnum form) is the pointer; everything else is the keyboard.
  if (!path) return EVDEV_EVENT0_MINOR;
  if (strstr(path, "event1") || strstr(path, "13:65") || strstr(path, "c13:65"))
    return EVDEV_EVENT1_MINOR;
  return EVDEV_EVENT0_MINOR;
}

void evdev_fake_stat(unsigned int* mode, unsigned long* rdev, const char* path) {
  int k = evdev_sysfs_kind(path);
  if (k == 1 || k == 2 || k == 4 || k == 5) { *mode = 0100000u; *rdev = 0; return; }  // S_IFREG
  if (k == 3) { *mode = 0120000u; *rdev = 0; return; }                                // S_IFLNK
  if (k == 6 || k == 7) { *mode = 0040000u; *rdev = 0; return; }                      // S_IFDIR
  // /dev/input/eventN char node: major 13, minor 64/65.
  int minor = evdev_minor_of_path(path);
  *mode = (*mode & 0777u) | 0020000u;                 // S_IFCHR
  *rdev = ((unsigned long)INPUT_MAJOR << 8) | (unsigned long)minor;
}

// Return:
//   0 = not ours
//   1 = <dev>/uevent          (S_IFREG)
//   2 = <dev>/dev             (S_IFREG)
//   3 = <dev>/subsystem       (S_IFLNK)
//   4 = /run/udev/data/c13:<minor>              (S_IFREG) legacy udev-db key
//   5 = /run/udev/data/+input:<name>            (S_IFREG) systemd>=255 udev-db key
//   6 = <dev>                 (S_IFDIR)  either /sys/class/input/eventN or /sys/dev/char/13:6N
//   7 = /sys/class/input      (S_IFDIR)
int evdev_sysfs_kind(const char* path) {
  if (!path) return 0;
  if (strcmp(path, "/sys/class/input") == 0) return 7;

  // Normalize a device node path to "event0"/"event1" (class form) or "13:64"/"13:65" (devnum form).
  const char* leaf = NULL;   // device basename + any trailing "/<file>"
  const char* dev = NULL;    // device basename ("event0"/"event1"/"13:64"/"13:65")
  if (strncmp(path, "/sys/class/input/event", 22) == 0) {
    leaf = path + sizeof("/sys/class/input/") - 1;
  } else if (strncmp(path, "/sys/dev/char/13:", sizeof("/sys/dev/char/13:") - 1) == 0) {
    leaf = path + sizeof("/sys/dev/char/") - 1;
  }
  if (leaf) {
    // device basename is up to the first '/'
    const char* p = leaf;
    while (*p && *p != '/') p++;
    size_t devlen = (size_t)(p - leaf);
    if (devlen == 6 && strncmp(leaf, "event0", 6) == 0) dev = "event0";
    else if (devlen == 6 && strncmp(leaf, "event1", 6) == 0) dev = "event1";
    else if (devlen == 5 && strncmp(leaf, "13:64", 5) == 0) dev = "event0";
    else if (devlen == 5 && strncmp(leaf, "13:65", 5) == 0) dev = "event1";
    if (dev) {
      if (*p == 0) return 6;
      if (strcmp(p, "/uevent") == 0) return 1;
      if (strcmp(p, "/dev") == 0) return 2;
      if (strcmp(p, "/subsystem") == 0) return 3;
    }
    return 0;
  }
  if (strcmp(path, "/run/udev/data/c13:64") == 0 || strcmp(path, "/run/udev/data/c13:65") == 0) return 4;
  if (strcmp(path, "/run/udev/data/+input:event0") == 0 || strcmp(path, "/run/udev/data/+input:event1") == 0) return 5;
  return 0;
}

const char* evdev_sysfs_content(const char* path) {
  static char buf[256];
  int minor = evdev_minor_of_path(path);
  int is_kbd = minor == EVDEV_EVENT0_MINOR;
  switch (evdev_sysfs_kind(path)) {
    case 1:
      snprintf(buf, sizeof buf,
               "MAJOR=%d\nMINOR=%d\nDEVNAME=input/event%d\nSUBSYSTEM=input\n",
               INPUT_MAJOR, minor, minor - EVDEV_EVENT0_MINOR);
      return buf;
    case 2:
      snprintf(buf, sizeof buf, "%d:%d\n", INPUT_MAJOR, minor);
      return buf;
    case 4: case 5:
      snprintf(buf, sizeof buf,
               "S:class/input/event%d\nE:ID_INPUT=1\nE:ID_INPUT_%s=1\nE:ID_SEAT=seat0\n",
               minor - EVDEV_EVENT0_MINOR, is_kbd ? "KEYBOARD" : "MOUSE");
      return buf;
    default: return 0;
  }
}

const char* evdev_sysfs_link(void) { return "/sys/class/input"; }
