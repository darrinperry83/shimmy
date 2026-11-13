// v4l2cam.c — present /dev/video0 so Linux camera software runs on this Mac.
//
// The Mac's camera is behind AVFoundation, which a Linux binary can reach through the hostcall
// (proved by tests/wl/lxgl.c for OpenGL and tests/wl/vtenc.c for the Media Engine). The Linux
// side of a camera is V4L2: a device node plus a well-documented ioctl set that every webcam
// program speaks — browsers, OBS, cheese. So the bridge is the same shape as drmasahi.c: present
// the node, answer the ioctls.
//
// V4L2 is a far better target than the asahi UAPI: it is a STABLE, public, documented interface,
// so the struct layouts below are ABI rather than guesses. Even so this starts as a LOGGER for
// everything past QUERYCAP, because what matters is the exact call SEQUENCE a real client issues
// (and in what order), which is not something to recall from memory.
//
// RDCAM=1        present /dev/video0
// RDCAM_TRACE=1  log every ioctl
#include "v4l2cam.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IOC_NR(x)   (((x) >> 0)  & 0xFF)
#define IOC_TYPE(x) (((x) >> 8)  & 0xFF)
#define IOC_SIZE(x) (((x) >> 16) & 0x3FFF)
#define IOC_DIR(x)  (((x) >> 30) & 0x3)
#define V4L2_TYPE 'V'

// struct v4l2_capability — 104 bytes, ABI:
//   0 driver[16]  16 card[32]  48 bus_info[32]  80 version(u32)
//  84 capabilities(u32)  88 device_caps(u32)  92 reserved[3]
struct v4l2_capability_abi {
  unsigned char driver[16], card[32], bus_info[32];
  uint32_t version, capabilities, device_caps, reserved[3];
};

#define V4L2_CAP_VIDEO_CAPTURE 0x00000001u
#define V4L2_CAP_READWRITE     0x01000000u
#define V4L2_CAP_STREAMING     0x04000000u
#define V4L2_CAP_DEVICE_CAPS   0x80000000u

static int g_on = -1, g_trace = -1;
#define MAXFD 32
static int g_fds[MAXFD]; static int g_nfd;
static int tracing(void);

int cam_enabled(void) {
  if (g_on < 0) { const char* e = getenv("RDCAM"); g_on = (e && *e && *e != '0'); }
  return g_on;
}
static int tracing(void) {
  if (g_trace < 0) { const char* e = getenv("RDCAM_TRACE"); g_trace = (e && *e && *e != '0'); }
  return g_trace;
}

int cam_is_node(const char* p) {
  if (!p || !cam_enabled()) return 0;
  int hit = strcmp(p, "/dev/video0") == 0;
  if (tracing() && !strncmp(p, "/dev/video", 10))
    fprintf(stderr, "[cam] path %-24s %s\n", p, hit ? "MATCH" : "(miss)");
  return hit;
}

void cam_register_fd(int fd) {
  if (fd < 0 || g_nfd >= MAXFD) return;
  g_fds[g_nfd++] = fd;
  if (tracing()) fprintf(stderr, "[cam] opened /dev/video0 as fd %d\n", fd);
}
void cam_forget_fd(int fd) {
  for (int i = 0; i < g_nfd; i++) if (g_fds[i] == fd) { g_fds[i] = g_fds[--g_nfd]; return; }
}
int cam_owns_fd(int fd) {
  for (int i = 0; i < g_nfd; i++) if (g_fds[i] == fd) return 1;
  return 0;
}

// V4L2 video devices are char major 81; minor 0 is video0. libv4l and most clients check this.
void cam_fake_stat(unsigned int* mode, unsigned long* rdev) {
  *mode = (*mode & 0777u) | 0020000u;                 // S_IFCHR
  *rdev = (81ul << 8) | 0ul;
}

static const char* nrname(unsigned nr) {
  switch (nr) {
    case 0:  return "QUERYCAP";     case 2:  return "ENUM_FMT";
    case 4:  return "G_FMT";        case 5:  return "S_FMT";
    case 8:  return "REQBUFS";      case 9:  return "QUERYBUF";
    case 15: return "QBUF";         case 17: return "DQBUF";
    case 18: return "STREAMON";     case 19: return "STREAMOFF";
    case 10: return "G_FBUF";       case 21: return "G_PARM";
    case 22: return "S_PARM";       case 26: return "G_CTRL";
    case 27: return "S_CTRL";       case 36: return "QUERYCTRL";
    case 70: return "ENUM_FRAMESIZES";
    case 71: return "ENUM_FRAMEINTERVALS";
    default: return "?";
  }
}

long cam_ioctl(int fd, unsigned long req, unsigned long arg) {
  unsigned dir = IOC_DIR(req), type = IOC_TYPE(req), nr = IOC_NR(req), sz = IOC_SIZE(req);

  if (type == V4L2_TYPE && nr == 0 && arg) {                 // VIDIOC_QUERYCAP
    struct v4l2_capability_abi* c = (struct v4l2_capability_abi*)arg;
    memset(c, 0, sizeof *c);
    snprintf((char*)c->driver, sizeof c->driver, "%s", "shimcam");
    snprintf((char*)c->card, sizeof c->card, "%s", "Apple Camera (AVFoundation)");
    snprintf((char*)c->bus_info, sizeof c->bus_info, "%s", "platform:shim");
    c->version = (6u << 16) | (1u << 8) | 0u;                // pretend 6.1.0
    c->device_caps  = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING | V4L2_CAP_READWRITE;
    c->capabilities = c->device_caps | V4L2_CAP_DEVICE_CAPS;
    if (tracing()) fprintf(stderr, "[cam] QUERYCAP -> shimcam / Apple Camera\n");
    return 0;
  }

  if (tracing())
    fprintf(stderr, "[cam] fd=%d %-18s dir=%u nr=%-3u size=%-4u -> -EINVAL (not implemented yet)\n",
            fd, type == V4L2_TYPE ? nrname(nr) : "non-V4L2", dir, nr, sz);
  return -22;                                                // -EINVAL
}
