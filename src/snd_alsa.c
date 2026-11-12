// snd_alsa.c — synthetic ALSA card for the Linux guest.
//
// Why: the Arch guest's PipeWire needs a sound card to produce audio. The shim runs the guest
// in-process, so a virtual char device whose ioctls this file answers is the natural fit — the
// exact technique drmasahi.c already uses for /dev/dri. libasound (and PipeWire's spa-alsa
// plugin) open /dev/snd/controlC0 + /dev/snd/pcmC0D0p, negotiate hw params, mmap the status /
// control / data pages, then poll() the PCM fd and write PCM into the mmap'd ring buffer.
//
// This file emulates that whole surface:
//   - /dev/snd/controlC0   (SNDRV_CTL_IOCTL_* — card enumeration for wireplumber's ALSA monitor)
//   - /dev/snd/pcmC0D0p    (playback; data is pulled by coreaudio_host.c)
//   - /dev/snd/pcmC0D0c    (capture; returns silence)
//   - /proc/asound/{cards,pcm,version,...}  (alsa-lib's snd_card_next reads these)
//
// The PCM ring buffer is a lock-free producer/consumer ring:
//   guest  (libasound):   writes PCM at appl_ptr, advances appl_ptr (mmap'd control page)
//   host   (CoreAudio):   reads PCM at hw_ptr, advances hw_ptr (mmap'd status page)
// All three pages (status/control/data) are MAP_SHARED anon-file mappings so the guest's own
// mmap aliases and this file's canonical mappings observe the same bytes. Struct layouts below
// are fixed-width and match the aarch64 (LP64) layouts in <sound/asound.h> exactly.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <pthread.h>
#include "snd_alsa.h"

// ---- ioctl decoding (Linux generic _IOC layout, identical to drmasahi.c) -----------------
#define IOC_NRSHIFT    0
#define IOC_TYPESHIFT  8
#define IOC_SIZESHIFT  16
#define IOC_DIRSHIFT   30
#define IOC_NR(x)   (((x) >> IOC_NRSHIFT)   & 0xFF)
#define IOC_TYPE(x) (((x) >> IOC_TYPESHIFT) & 0xFF)
#define IOC_SIZE(x) (((x) >> IOC_SIZESHIFT) & 0x3FFF)
#define IOC_DIR(x)  (((x) >> IOC_DIRSHIFT)  & 0x3)

// ALSA ioctl type bytes.
#define SND_TYPE_PCM    'A'
#define SND_TYPE_CTL    'U'

// Linux errnos we reference directly.
#define LX_EINVAL     22
#define LX_ENODEV     19
#define LX_ENOTTY     25
#define LX_EPIPE      32
#define LX_EIO         5
#define LX_EFAULT     14

// ---- ALSA UAPI enums/constants (values from <sound/asound.h>) ---------------------------
#define SNDRV_PCM_VERSION  ((2u<<16)|(0u<<8)|18u)   // 2.0.18
#define SNDRV_CTL_VERSION  ((2u<<16)|(0u<<8)|9u)    // 2.0.9

#define SNDRV_PCM_STREAM_PLAYBACK 0
#define SNDRV_PCM_STREAM_CAPTURE  1

#define SNDRV_PCM_STATE_OPEN        0
#define SNDRV_PCM_STATE_SETUP       1
#define SNDRV_PCM_STATE_PREPARED    2
#define SNDRV_PCM_STATE_RUNNING     3
#define SNDRV_PCM_STATE_XRUN        4
#define SNDRV_PCM_STATE_DRAINING    5
#define SNDRV_PCM_STATE_PAUSED      6
#define SNDRV_PCM_STATE_SUSPENDED   7

#define SNDRV_PCM_CLASS_GENERIC     0
#define SNDRV_PCM_SUBCLASS_GENERIC_MIX 0

#define SNDRV_PCM_ACCESS_MMAP_INTERLEAVED   0
#define SNDRV_PCM_ACCESS_MMAP_NONINTERLEAVED 1
#define SNDRV_PCM_ACCESS_RW_INTERLEAVED     3
#define SNDRV_PCM_ACCESS_RW_NONINTERLEAVED  4

#define SNDRV_PCM_FORMAT_S16_LE     2
#define SNDRV_PCM_FORMAT_S32_LE     10
#define SNDRV_PCM_FORMAT_FLOAT_LE   14

#define SNDRV_PCM_SUBFORMAT_STD     0

#define SNDRV_PCM_INFO_MMAP            0x00000001u
#define SNDRV_PCM_INFO_MMAP_VALID      0x00000002u
#define SNDRV_PCM_INFO_INTERLEAVED     0x00000100u
#define SNDRV_PCM_INFO_NONINTERLEAVED  0x00000200u
#define SNDRV_PCM_INFO_PAUSE           0x00080000u
#define SNDRV_PCM_INFO_RESUME          0x00040000u
#define SNDRV_PCM_INFO_SYNC_APPLPTR    0x00000020u
#define SNDRV_PCM_INFO_PERFECT_DRAIN   0x00000040u
#define SNDRV_PCM_INFO_HAS_WALL_CLOCK  0x01000000u

#define SNDRV_PCM_MMAP_OFFSET_DATA      0x00000000u
#define SNDRV_PCM_MMAP_OFFSET_STATUS    0x80000000u
#define SNDRV_PCM_MMAP_OFFSET_CONTROL   0x81000000u

#define SNDRV_PCM_SYNC_PTR_HWSYNC   (1u<<0)
#define SNDRV_PCM_SYNC_PTR_APPL     (1u<<1)
#define SNDRV_PCM_SYNC_PTR_AVAIL_MIN (1u<<2)

// hw param indices
#define SNDRV_PCM_HW_PARAM_ACCESS      0
#define SNDRV_PCM_HW_PARAM_FORMAT      1
#define SNDRV_PCM_HW_PARAM_SUBFORMAT   2
#define SNDRV_PCM_HW_PARAM_FIRST_MASK  0
#define SNDRV_PCM_HW_PARAM_LAST_MASK   2
#define SNDRV_PCM_HW_PARAM_SAMPLE_BITS 8
#define SNDRV_PCM_HW_PARAM_FRAME_BITS  9
#define SNDRV_PCM_HW_PARAM_CHANNELS    10
#define SNDRV_PCM_HW_PARAM_RATE        11
#define SNDRV_PCM_HW_PARAM_PERIOD_TIME 12
#define SNDRV_PCM_HW_PARAM_PERIOD_SIZE 13
#define SNDRV_PCM_HW_PARAM_PERIOD_BYTES 14
#define SNDRV_PCM_HW_PARAM_PERIODS     15
#define SNDRV_PCM_HW_PARAM_BUFFER_TIME 16
#define SNDRV_PCM_HW_PARAM_BUFFER_SIZE 17
#define SNDRV_PCM_HW_PARAM_BUFFER_BYTES 18
#define SNDRV_PCM_HW_PARAM_TICK_TIME   19
#define SNDRV_PCM_HW_PARAM_FIRST_INTERVAL 8
#define SNDRV_PCM_HW_PARAM_LAST_INTERVAL  19

#define SNDRV_CTL_POWER_D0 0x0000

#define SNDRV_CTL_ELEM_IFACE_MIXER 2

// ---- PCM ioctl nr values (SNDRV_PCM_IOCTL_*) ------------------------------------------
#define NR_PCM_PVERSION      0x00
#define NR_PCM_INFO          0x01
#define NR_PCM_TSTAMP        0x02
#define NR_PCM_TTSTAMP       0x03
#define NR_PCM_USER_PVERSION 0x04
#define NR_PCM_HW_REFINE     0x10
#define NR_PCM_HW_PARAMS     0x11
#define NR_PCM_HW_FREE       0x12
#define NR_PCM_SW_PARAMS     0x13
#define NR_PCM_STATUS        0x20
#define NR_PCM_DELAY         0x21
#define NR_PCM_HWSYNC        0x22
#define NR_PCM_SYNC_PTR      0x23
#define NR_PCM_STATUS_EXT    0x24
#define NR_PCM_CHANNEL_INFO  0x32
#define NR_PCM_PREPARE       0x40
#define NR_PCM_RESET         0x41
#define NR_PCM_START         0x42
#define NR_PCM_DROP          0x43
#define NR_PCM_DRAIN         0x44
#define NR_PCM_PAUSE         0x45
#define NR_PCM_REWIND        0x46
#define NR_PCM_RESUME        0x47
#define NR_PCM_XRUN          0x48
#define NR_PCM_FORWARD       0x49
#define NR_PCM_WRITEI_FRAMES 0x50
#define NR_PCM_READI_FRAMES  0x51
#define NR_PCM_LINK          0x60
#define NR_PCM_UNLINK        0x61

// ---- CTL ioctl nr values (SNDRV_CTL_IOCTL_*) ------------------------------------------
#define NR_CTL_PVERSION      0x00
#define NR_CTL_CARD_INFO     0x01
#define NR_CTL_ELEM_LIST     0x10
#define NR_CTL_ELEM_INFO     0x11
#define NR_CTL_ELEM_READ     0x12
#define NR_CTL_ELEM_WRITE    0x13
#define NR_CTL_SUBSCRIBE_EVENTS 0x16
#define NR_CTL_HWDEP_NEXT_DEVICE 0x20
#define NR_CTL_PCM_NEXT_DEVICE 0x30
#define NR_CTL_PCM_INFO      0x31
#define NR_CTL_PCM_PREFER_SUBDEVICE 0x32
#define NR_CTL_RAWMIDI_NEXT_DEVICE 0x40
#define NR_CTL_POWER         0xd0
#define NR_CTL_POWER_STATE   0xd1

// ---- fixed-width struct layouts (aarch64 LP64 == macOS arm64 LP64) ---------------------
typedef int32_t  snd_pcm_state_t;
typedef uint64_t snd_pcm_uframes_t;
typedef int64_t  snd_pcm_sframes_t;
typedef uint64_t __kernel_off_t;

struct snd_timespec { int64_t tv_sec; int64_t tv_nsec; };

struct snd_interval {
  uint32_t min, max;
  uint32_t openmin:1, openmax:1, integer:1, empty:1;
};
#define SNDRV_MASK_MAX 256
struct snd_mask { uint32_t bits[(SNDRV_MASK_MAX+31)/32]; };  // 8 * u32

struct snd_pcm_hw_params {
  uint32_t flags;
  struct snd_mask masks[3];
  struct snd_mask mres[5];
  struct snd_interval intervals[12];
  struct snd_interval ires[9];
  uint32_t rmask;
  uint32_t cmask;
  uint32_t info;
  uint32_t msbits;
  uint32_t rate_num;
  uint32_t rate_den;
  snd_pcm_uframes_t fifo_size;
  unsigned char sync[16];
  unsigned char reserved[48];
};

struct snd_pcm_sw_params {
  int32_t tstamp_mode;
  uint32_t period_step;
  uint32_t sleep_min;
  snd_pcm_uframes_t avail_min;
  snd_pcm_uframes_t xfer_align;
  snd_pcm_uframes_t start_threshold;
  snd_pcm_uframes_t stop_threshold;
  snd_pcm_uframes_t silence_threshold;
  snd_pcm_uframes_t silence_size;
  snd_pcm_uframes_t boundary;
  uint32_t proto;
  uint32_t tstamp_type;
  unsigned char reserved[56];
};

struct snd_pcm_info {
  uint32_t device;
  uint32_t subdevice;
  int32_t stream;
  int32_t card;
  unsigned char id[64];
  unsigned char name[80];
  unsigned char subname[32];
  int32_t dev_class;
  int32_t dev_subclass;
  uint32_t subdevices_count;
  uint32_t subdevices_avail;
  unsigned char pad1[16];
  unsigned char reserved[64];
};

struct snd_pcm_channel_info {
  uint32_t channel;
  __kernel_off_t offset;
  uint32_t first;
  uint32_t step;
};

struct snd_pcm_status {
  int32_t state;
  int32_t pad1;
  struct snd_timespec trigger_tstamp;
  struct snd_timespec tstamp;
  snd_pcm_uframes_t appl_ptr;
  snd_pcm_uframes_t hw_ptr;
  snd_pcm_sframes_t delay;
  snd_pcm_uframes_t avail;
  snd_pcm_uframes_t avail_max;
  snd_pcm_uframes_t overrange;
  int32_t suspended_state;
  uint32_t audio_tstamp_data;
  struct snd_timespec audio_tstamp;
  struct snd_timespec driver_tstamp;
  uint32_t audio_tstamp_accuracy;
  unsigned char reserved[20];
};

struct snd_pcm_mmap_status {
  int32_t state;
  int32_t pad1;
  snd_pcm_uframes_t hw_ptr;
  struct snd_timespec tstamp;
  int32_t suspended_state;
  int32_t pad2;
  struct snd_timespec audio_tstamp;
};

struct snd_pcm_mmap_control {
  snd_pcm_uframes_t appl_ptr;
  snd_pcm_uframes_t avail_min;
};

struct snd_pcm_sync_ptr {
  uint32_t flags;
  uint32_t pad1;
  union { struct snd_pcm_mmap_status status; unsigned char reserved[64]; } s;
  union { struct snd_pcm_mmap_control control; unsigned char reserved[64]; } c;
};

struct snd_xferi {
  snd_pcm_sframes_t result;
  void* buf;
  snd_pcm_uframes_t frames;
};

struct snd_ctl_card_info {
  int32_t card;
  int32_t pad;
  unsigned char id[16];
  unsigned char driver[16];
  unsigned char name[32];
  unsigned char longname[80];
  unsigned char reserved_[16];
  unsigned char mixername[80];
  unsigned char components[128];
};

struct snd_ctl_elem_id {
  uint32_t numid;
  int32_t iface;
  uint32_t device;
  uint32_t subdevice;
  unsigned char name[44];
  uint32_t index;
};

struct snd_ctl_elem_list {
  uint32_t offset;
  uint32_t space;
  uint32_t used;
  uint32_t count;
  struct snd_ctl_elem_id* pids;
  unsigned char reserved[50];
};

struct snd_ctl_elem_info {
  struct snd_ctl_elem_id id;
  int32_t type;
  uint32_t access;
  uint32_t count;
  int32_t owner;
  unsigned char value_reserved[128];
  unsigned char reserved[64];
};

// Layout self-checks (host LP64 == guest aarch64 LP64). These are compile-time.
_Static_assert(sizeof(struct snd_interval) == 12, "snd_interval layout");
_Static_assert(sizeof(struct snd_mask) == 32, "snd_mask layout");
_Static_assert(sizeof(struct snd_pcm_hw_params) == 608, "hw_params size");
_Static_assert(sizeof(struct snd_pcm_info) == 288, "snd_pcm_info size");
_Static_assert(sizeof(struct snd_pcm_sw_params) == 136, "sw_params size");
_Static_assert(sizeof(struct snd_pcm_status) == 152, "snd_pcm_status size");
_Static_assert(sizeof(struct snd_pcm_mmap_status) == 56, "mmap_status size");
_Static_assert(sizeof(struct snd_pcm_mmap_control) == 16, "mmap_control size");
_Static_assert(sizeof(struct snd_ctl_card_info) == 376, "ctl_card_info size");

// ---- device identity / fd registry ----------------------------------------------------
#define SND_MAJOR 116

// forward decls: the stream machinery lives below, but the fd registry needs to release it.
typedef struct snd_stream snd_stream;
static void stream_free(snd_stream* s);
static void stream_alloc_pages(snd_stream* s);
static int  stream_alloc_data(snd_stream* s);
static void stream_init(snd_stream* s, int dir);
static snd_stream g_play, g_cap;   // defined (initialized) below

static int g_on = -1, g_trace = -1;
#define MAXFD 64
typedef struct { int fd; int dir; } snd_fdent;   // dir: -1 control, 0 playback, 1 capture
static snd_fdent g_fds[MAXFD];
static int g_nfd = 0;

int snd_enabled(void) {
  if (g_on < 0) { const char* e = getenv("RDSND"); g_on = (e && *e && *e != '0'); }
  return g_on;
}
static int tracing(void) {
  if (g_trace < 0) { const char* e = getenv("RDSND_TRACE"); g_trace = (e && *e && *e != '0'); }
  return g_trace;
}

int snd_is_node(const char* p) {
  if (!p || !snd_enabled()) return 0;
  int hit = strcmp(p, "/dev/snd/controlC0") == 0 ||
            strcmp(p, "/dev/snd/pcmC0D0p") == 0 ||
            strcmp(p, "/dev/snd/pcmC0D0c") == 0 ||
            strcmp(p, "/dev/snd/pcmC0D1p") == 0 ||
            strcmp(p, "/dev/snd/pcmC0D1c") == 0;
  if (tracing() && !strncmp(p, "/dev/snd", 8))
    fprintf(stderr, "[snd] path %-28s %s\n", p, hit ? "MATCH" : "(miss)");
  return hit;
}

static int dir_by_fd(int fd) {
  for (int i = 0; i < g_nfd; i++) if (g_fds[i].fd == fd) return g_fds[i].dir;
  return -2;
}
static void tag_fd(int fd, int dir) {
  if (fd < 0) return;
  for (int i = 0; i < g_nfd; i++) if (g_fds[i].fd == fd) { g_fds[i].dir = dir; return; }
  if (g_nfd < MAXFD) { g_fds[g_nfd].fd = fd; g_fds[g_nfd].dir = dir; g_nfd++; }
}
void snd_register_fd(int fd) { tag_fd(fd, -1); if (tracing()) fprintf(stderr, "[snd] opened control fd %d\n", fd); }
void snd_assign_fd(int fd, int dir) {
  tag_fd(fd, dir);
  stream_init(dir == 0 ? &g_play : &g_cap, dir);
  if (tracing()) fprintf(stderr, "[snd] opened pcm fd %d dir=%d\n", fd, dir);
}
void snd_dup_fd(int oldfd, int newfd) {
  int d = dir_by_fd(oldfd);
  if (d >= -1) tag_fd(newfd, d);
}
void snd_forget_fd(int fd) {
  int d = dir_by_fd(fd);
  for (int i = 0; i < g_nfd; i++)
    if (g_fds[i].fd == fd) { g_fds[i] = g_fds[--g_nfd]; break; }
  if (d == 0) { ca_playback_stop(); stream_free(&g_play); }
  else if (d == 1) stream_free(&g_cap);
}
int snd_owns_fd(int fd) { return dir_by_fd(fd) >= -1; }

void snd_fake_stat(unsigned int* mode, unsigned long* rdev, const char* path) {
  (void)path;
  *mode = (*mode & 0777u) | 0020000u;               // S_IFCHR
  unsigned minor = 0;
  if (strstr(path, "pcmC0D0p") || strstr(path, "pcmC0D1p")) minor = 16;
  else if (strstr(path, "pcmC0D0c") || strstr(path, "pcmC0D1c")) minor = 17;
  else if (strstr(path, "control")) minor = 0;
  *rdev = ((unsigned long)SND_MAJOR << 8) | minor;
}

int snd_sysfs_kind(const char* path) {
  // Minimal: no synthetic /sys/class/sound nodes needed by alsa-lib/wireplumber for the
  // enumeration path we exercise. Kept as a hook for future expansion.
  (void)path;
  return 0;
}

const char* snd_proc_content(const char* path) {
  static char buf[1024];
  if (strcmp(path, "/proc/asound/cards") == 0)
    return " 0 [rdmetal        ]: rdmetal - rdmetal virtual audio\n                    rdmetal virtual audio\n";
  if (strcmp(path, "/proc/asound/pcm") == 0)
    return "00-00: rdmetal playback : rdmetal playback : playback 1\n"
           "00-01: rdmetal capture : rdmetal capture : capture 1\n";
  if (strcmp(path, "/proc/asound/version") == 0)
    return "Advanced Linux Sound Architecture Driver Version k6.6.0-rdmetal.\n";
  if (strcmp(path, "/proc/asound/devices") == 0)
    return "  0: [ 0]   : control\n"
           " 16: [ 0- 0]: digital audio playback\n"
           " 17: [ 0- 1]: digital audio capture\n"
           " 33:        : timer\n";
  if (strcmp(path, "/proc/asound/oss/devices") == 0 || strcmp(path, "/proc/asound/oss/sndstat") == 0)
    return "rdmetal virtual audio\n";
  if (strcmp(path, "/proc/asound/timers") == 0)
    return "G0: system timer : 1000.000us (10000000 ticks)\n";
  snprintf(buf, sizeof buf, "total %d\ndrwxr-xr-x 1 root root 0 Jan  1  1970 .\n", 0);
  return buf;
}

// ---- stream state ----------------------------------------------------------------------
typedef struct snd_stream {
  int in_use;
  int dir;                 // SNDRV_PCM_STREAM_*
  // negotiated hw params
  uint32_t access;
  uint32_t format;
  uint32_t channels;
  uint32_t rate;
  snd_pcm_uframes_t period_size;
  snd_pcm_uframes_t buffer_size;
  size_t frame_bytes;
  // sw params
  snd_pcm_uframes_t boundary;
  snd_pcm_uframes_t avail_min;
  snd_pcm_uframes_t start_threshold;
  // shared pages (canonical MAP_SHARED mappings; guest mmaps get fresh aliases)
  int status_fd;  struct snd_pcm_mmap_status* status;
  int control_fd; struct snd_pcm_mmap_control* control;
  int data_fd;    void* data; size_t data_len;
  int state;       // SNDRV_PCM_STATE_*
  int prepared;
} snd_stream;

static snd_stream g_play = {0};
static snd_stream g_cap  = {0};

static snd_stream* stream_by_fd(int fd) {
  int d = dir_by_fd(fd);
  if (d == 0) return &g_play;
  if (d == 1) return &g_cap;
  return NULL;
}

// anon-file-backed page(s): survives guest munmap, shares bytes across aliases
static void* anon_map(size_t len, int* fd_out) {
  char tmpl[64] = "/tmp/rdsnd-XXXXXX";
  int fd = mkstemp(tmpl);
  if (fd < 0) return NULL;
  unlink(tmpl);
  if (ftruncate(fd, (off_t)len) < 0) { close(fd); return NULL; }
  void* m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (m == MAP_FAILED) { close(fd); return NULL; }
  if (fd_out) *fd_out = fd; else close(fd);
  return m;
}

static void stream_alloc_pages(snd_stream* s) {
  if (!s->status) {
    s->status = anon_map(4096, &s->status_fd);
    if (s->status) { memset(s->status, 0, 4096); s->status->state = SNDRV_PCM_STATE_OPEN; }
  }
  if (!s->control) {
    s->control = anon_map(4096, &s->control_fd);
    if (s->control) memset(s->control, 0, 4096);
  }
}

static void stream_free(snd_stream* s) {
  if (s->data)   { munmap(s->data, s->data_len); s->data = NULL; }
  if (s->data_fd >= 0) { close(s->data_fd); s->data_fd = -1; }
  if (s->status) { munmap(s->status, 4096); s->status = NULL; }
  if (s->status_fd >= 0) { close(s->status_fd); s->status_fd = -1; }
  if (s->control) { munmap(s->control, 4096); s->control = NULL; }
  if (s->control_fd >= 0) { close(s->control_fd); s->control_fd = -1; }
  s->data_len = 0;
  s->prepared = 0;
  s->state = SNDRV_PCM_STATE_OPEN;
}

// Initialize a stream (one per direction) when a pcm node is opened.
static void stream_init(snd_stream* s, int dir) {
  s->in_use = 1;
  s->dir = dir;
  s->access = SNDRV_PCM_ACCESS_MMAP_INTERLEAVED;
  s->format = SNDRV_PCM_FORMAT_S16_LE;
  s->channels = 2;
  s->rate = 48000;
  s->period_size = 1024;
  s->buffer_size = 4096;
  s->frame_bytes = 4;                       // S16 stereo
  s->boundary = 4096;
  s->avail_min = s->period_size;
  s->start_threshold = s->buffer_size;
  s->state = SNDRV_PCM_STATE_OPEN;
  s->prepared = 0;
  stream_alloc_pages(s);
}

// ---- interval / mask helpers -----------------------------------------------------------
static int ival_empty(const struct snd_interval* i) {
  return i->min > i->max || (i->min == i->max && (i->openmin || i->openmax));
}
static void ival_set(struct snd_interval* i, uint32_t lo, uint32_t hi, int openlo, int openhi) {
  i->min = lo; i->max = hi; i->openmin = openlo; i->openmax = openhi;
  i->integer = 1; i->empty = 0;
}
static void ival_intersect(struct snd_interval* o, const struct snd_interval* a, const struct snd_interval* b) {
  uint32_t lo = a->min > b->min ? a->min : b->min;
  uint32_t hi = a->max < b->max ? a->max : b->max;
  int olo = (a->min > b->min) ? a->openmin : (a->min < b->min ? b->openmin : (a->openmin || b->openmin));
  int ohi = (a->max < b->max) ? a->openmax : (a->max > b->max ? b->openmax : (a->openmax || b->openmax));
  o->min = lo; o->max = hi; o->openmin = olo; o->openmax = ohi;
  o->integer = a->integer || b->integer; o->empty = 0;
}
static int mask_empty(const struct snd_mask* m) {
  for (int i = 0; i < 8; i++) if (m->bits[i]) return 0;
  return 1;
}
static void mask_set_bit(struct snd_mask* m, int bit) { m->bits[bit >> 5] |= 1u << (bit & 31); }
static void mask_and(struct snd_mask* o, const struct snd_mask* a, const struct snd_mask* b) {
  for (int i = 0; i < 8; i++) o->bits[i] = a->bits[i] & b->bits[i];
}

// The hardware capability we report.
static void cap_mask(struct snd_mask* m, int which) {
  memset(m, 0, sizeof *m);
  if (which == SNDRV_PCM_HW_PARAM_ACCESS) {
    mask_set_bit(m, SNDRV_PCM_ACCESS_MMAP_INTERLEAVED);
    mask_set_bit(m, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
  } else if (which == SNDRV_PCM_HW_PARAM_FORMAT) {
    mask_set_bit(m, SNDRV_PCM_FORMAT_S16_LE);
    mask_set_bit(m, SNDRV_PCM_FORMAT_S32_LE);
    mask_set_bit(m, SNDRV_PCM_FORMAT_FLOAT_LE);
  } else if (which == SNDRV_PCM_HW_PARAM_SUBFORMAT) {
    mask_set_bit(m, SNDRV_PCM_SUBFORMAT_STD);
  }
}
static void cap_ival(struct snd_interval* i, int which) {
  switch (which) {
    case SNDRV_PCM_HW_PARAM_SAMPLE_BITS:  ival_set(i, 16, 32, 0, 0); break;
    case SNDRV_PCM_HW_PARAM_FRAME_BITS:   ival_set(i, 16, 64, 0, 0); break;
    case SNDRV_PCM_HW_PARAM_CHANNELS:     ival_set(i, 1, 2, 0, 0); break;
    case SNDRV_PCM_HW_PARAM_RATE:         ival_set(i, 48000, 48000, 0, 0); break;
    case SNDRV_PCM_HW_PARAM_PERIOD_TIME:  ival_set(i, 1000, 1000000, 0, 0); break;
    case SNDRV_PCM_HW_PARAM_PERIOD_SIZE:  ival_set(i, 16, 4096, 0, 0); break;
    case SNDRV_PCM_HW_PARAM_PERIOD_BYTES: ival_set(i, 64, 65536, 0, 0); break;
    case SNDRV_PCM_HW_PARAM_PERIODS:      ival_set(i, 2, 1024, 0, 0); break;
    case SNDRV_PCM_HW_PARAM_BUFFER_TIME:  ival_set(i, 1000, 10000000, 0, 0); break;
    case SNDRV_PCM_HW_PARAM_BUFFER_SIZE:  ival_set(i, 32, 65536, 0, 0); break;
    case SNDRV_PCM_HW_PARAM_BUFFER_BYTES: ival_set(i, 128, 1048576, 0, 0); break;
    case SNDRV_PCM_HW_PARAM_TICK_TIME:    ival_set(i, 1000, 1000000, 0, 0); break;
    default: memset(i, 0, sizeof *i); i->empty = 1; break;
  }
}

// fill a whole snd_pcm_hw_params with the capability (used to answer HW_REFINE wildcard)
static void fill_capability(struct snd_pcm_hw_params* hw) {
  memset(hw, 0, sizeof *hw);
  for (int k = SNDRV_PCM_HW_PARAM_FIRST_MASK; k <= SNDRV_PCM_HW_PARAM_LAST_MASK; k++)
    cap_mask(&hw->masks[k], k);
  for (int k = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; k <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; k++)
    cap_ival(&hw->intervals[k - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL], k);
}

// ---- PCM ioctl handlers ----------------------------------------------------------------
static int sample_bytes(uint32_t fmt) {
  if (fmt == SNDRV_PCM_FORMAT_S16_LE) return 2;
  if (fmt == SNDRV_PCM_FORMAT_S32_LE || fmt == SNDRV_PCM_FORMAT_FLOAT_LE) return 4;
  return 2;
}

static void fill_pcm_info(struct snd_pcm_info* info, int dir) {
  memset(info, 0, sizeof *info);
  info->device = 0;
  info->subdevice = 0;
  info->stream = dir;
  info->card = 0;
  memcpy(info->id, "rdmetal", 8);
  if (dir == SNDRV_PCM_STREAM_PLAYBACK) {
    memcpy(info->name, "rdmetal playback", 17);
    memcpy(info->subname, "rdmetal playback", 17);
  } else {
    memcpy(info->name, "rdmetal capture", 16);
    memcpy(info->subname, "rdmetal capture", 16);
  }
  info->dev_class = SNDRV_PCM_CLASS_GENERIC;
  info->dev_subclass = SNDRV_PCM_SUBCLASS_GENERIC_MIX;
  info->subdevices_count = 1;
  info->subdevices_avail = 1;
}

// Refine: intersect the caller's constraints with our capability.
static long pcm_hw_refine(struct snd_pcm_hw_params* hw) {
  struct snd_pcm_hw_params cap;
  fill_capability(&cap);
  // masks: empty (all-zero) input means wildcard -> capability
  for (int k = SNDRV_PCM_HW_PARAM_FIRST_MASK; k <= SNDRV_PCM_HW_PARAM_LAST_MASK; k++) {
    struct snd_mask* in = &hw->masks[k];
    struct snd_mask* c  = &cap.masks[k];
    if (mask_empty(in)) { *in = *c; }
    else { struct snd_mask t; mask_and(&t, in, c); if (mask_empty(&t)) return -LX_EINVAL; *in = t; }
  }
  for (int k = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; k <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; k++) {
    struct snd_interval* in = &hw->intervals[k - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    struct snd_interval* c  = &cap.intervals[k - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    if (in->empty) { *in = *c; }
    else { struct snd_interval t; ival_intersect(&t, in, c); if (ival_empty(&t)) return -LX_EINVAL; *in = t; }
  }
  hw->rmask = 0;
  hw->cmask = 0;
  hw->info = 0;
  hw->msbits = 0;
  hw->rate_num = 48000;
  hw->rate_den = 1;
  hw->fifo_size = 0;
  return 0;
}

// Pick a concrete value from a (possibly still ranged) interval.
static uint32_t ival_pick(const struct snd_interval* i, uint32_t def) {
  if (i->empty || ival_empty(i)) return def;
  if (i->min > 0) return i->min;
  if (i->max == i->min) return i->min;
  return i->max ? i->max : def;
}

static long pcm_hw_params(snd_stream* s, struct snd_pcm_hw_params* hw) {
  long r = pcm_hw_refine(hw);
  if (r < 0) return r;
  uint32_t access = 0;
  if (hw->masks[SNDRV_PCM_HW_PARAM_ACCESS].bits[0] & (1u << SNDRV_PCM_ACCESS_MMAP_INTERLEAVED))
    access = SNDRV_PCM_ACCESS_MMAP_INTERLEAVED;
  else if (hw->masks[SNDRV_PCM_HW_PARAM_ACCESS].bits[0] & (1u << SNDRV_PCM_ACCESS_RW_INTERLEAVED))
    access = SNDRV_PCM_ACCESS_RW_INTERLEAVED;
  else return -LX_EINVAL;

  uint32_t format = 0;
  const uint32_t* fb = hw->masks[SNDRV_PCM_HW_PARAM_FORMAT].bits;
  if (fb[0] & (1u << SNDRV_PCM_FORMAT_S16_LE)) format = SNDRV_PCM_FORMAT_S16_LE;
  else if (fb[0] & (1u << SNDRV_PCM_FORMAT_S32_LE)) format = SNDRV_PCM_FORMAT_S32_LE;
  else if (fb[0] & (1u << SNDRV_PCM_FORMAT_FLOAT_LE)) format = SNDRV_PCM_FORMAT_FLOAT_LE;
  else return -LX_EINVAL;

  uint32_t channels = ival_pick(&hw->intervals[SNDRV_PCM_HW_PARAM_CHANNELS - 8], 2);
  uint32_t rate     = ival_pick(&hw->intervals[SNDRV_PCM_HW_PARAM_RATE - 8], 48000);
  snd_pcm_uframes_t period = ival_pick(&hw->intervals[SNDRV_PCM_HW_PARAM_PERIOD_SIZE - 8], 1024);
  snd_pcm_uframes_t buffer = ival_pick(&hw->intervals[SNDRV_PCM_HW_PARAM_BUFFER_SIZE - 8], period * 4);
  if (channels < 1 || channels > 2) return -LX_EINVAL;
  if (period == 0) period = 1024;
  if (buffer < period * 2) buffer = period * 4;

  s->access = access;
  s->format = format;
  s->channels = channels;
  s->rate = rate;
  s->period_size = period;
  s->buffer_size = buffer;
  s->frame_bytes = (size_t)sample_bytes(format) * channels;
  s->boundary = buffer;
  s->state = SNDRV_PCM_STATE_SETUP;
  s->prepared = 0;
  stream_alloc_data(s);       // libasound maps the DATA page right after HW_PARAMS

  hw->info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID | SNDRV_PCM_INFO_INTERLEAVED |
             SNDRV_PCM_INFO_PAUSE | SNDRV_PCM_INFO_RESUME | SNDRV_PCM_INFO_PERFECT_DRAIN;
  hw->msbits = (format == SNDRV_PCM_FORMAT_S16_LE) ? 16 : 32;
  hw->rate_num = rate;
  hw->rate_den = 1;
  hw->fifo_size = 0;
  if (tracing())
    fprintf(stderr, "[snd] hw_params dir=%d access=%u fmt=%u ch=%u rate=%u period=%llu buffer=%llu\n",
            s->dir, access, format, channels, rate,
            (unsigned long long)period, (unsigned long long)buffer);
  return 0;
}

static long pcm_sw_params(snd_stream* s, struct snd_pcm_sw_params* sw) {
  s->avail_min = sw->avail_min ? sw->avail_min : s->period_size;
  s->start_threshold = sw->start_threshold ? sw->start_threshold : s->buffer_size;
  if (sw->boundary && sw->boundary > s->buffer_size) s->boundary = sw->boundary;
  if (s->control) s->control->avail_min = s->avail_min;
  return 0;
}

static void stream_zero_counters(snd_stream* s) {
  if (s->status) { s->status->hw_ptr = 0; s->status->state = SNDRV_PCM_STATE_PREPARED; }
  if (s->control) s->control->appl_ptr = 0;
}

// (Re)allocate the data ring buffer for the negotiated geometry. libasound maps the DATA page
// during snd_pcm_hw_params (before PREPARE), so this must be valid as soon as hw params are set.
static int stream_alloc_data(snd_stream* s) {
  size_t need = (size_t)s->buffer_size * s->frame_bytes;
  need = (need + 4095) & ~(size_t)4095;
  if (need < 4096) need = 4096;
  if (s->data && s->data_len >= need) { memset(s->data, 0, s->data_len); return 0; }
  if (s->data) { munmap(s->data, s->data_len); s->data = NULL; }
  if (s->data_fd >= 0) { close(s->data_fd); s->data_fd = -1; }
  s->data = anon_map(need, &s->data_fd);
  if (!s->data) return -LX_EINVAL;
  s->data_len = need;
  return 0;
}

static long pcm_prepare(snd_stream* s) {
  if (!s->in_use) return -LX_ENODEV;
  if (stream_alloc_data(s) < 0) return -LX_EINVAL;
  stream_zero_counters(s);
  s->prepared = 1;
  s->state = SNDRV_PCM_STATE_PREPARED;
  if (tracing()) fprintf(stderr, "[snd] prepare dir=%d buffer=%zu bytes\n", s->dir, s->data_len);
  return 0;
}

static snd_pcm_uframes_t avail_playback(snd_stream* s) {
  snd_pcm_uframes_t hw = s->status ? s->status->hw_ptr : 0;
  snd_pcm_uframes_t ap = s->control ? s->control->appl_ptr : 0;
  snd_pcm_uframes_t boundary = s->boundary ? s->boundary : s->buffer_size;
  // free space = buffer_size - (appl - hw), all modulo boundary
  snd_pcm_uframes_t used = (ap - hw) % boundary;
  if (used > s->buffer_size) used = s->buffer_size;   // clamp underflow/overflow
  return s->buffer_size - used;
}
static snd_pcm_uframes_t avail_capture(snd_stream* s) {
  snd_pcm_uframes_t hw = s->status ? s->status->hw_ptr : 0;
  snd_pcm_uframes_t ap = s->control ? s->control->appl_ptr : 0;
  snd_pcm_uframes_t boundary = s->boundary ? s->boundary : s->buffer_size;
  snd_pcm_uframes_t used = (hw - ap) % boundary;
  if (used > s->buffer_size) used = s->buffer_size;
  return used;
}

static long pcm_status(snd_stream* s, struct snd_pcm_status* st) {
  memset(st, 0, sizeof *st);
  st->state = s->status ? s->status->state : s->state;
  st->appl_ptr = s->control ? s->control->appl_ptr : 0;
  st->hw_ptr   = s->status ? s->status->hw_ptr : 0;
  snd_pcm_uframes_t avail = (s->dir == SNDRV_PCM_STREAM_PLAYBACK) ? avail_playback(s) : avail_capture(s);
  snd_pcm_uframes_t used = s->buffer_size - avail;
  st->avail = avail;
  st->avail_max = s->buffer_size;
  st->delay = (s->dir == SNDRV_PCM_STREAM_PLAYBACK) ? (snd_pcm_sframes_t)used : 0;
  st->overrange = 0;
  st->suspended_state = SNDRV_PCM_STATE_OPEN;
  return 0;
}

static long pcm_sync_ptr(snd_stream* s, struct snd_pcm_sync_ptr* sp) {
  if (sp->flags & SNDRV_PCM_SYNC_PTR_HWSYNC) {
    // copy guest appl_ptr/avail_min from the caller's control view into ours
    if (s->control) {
      if (sp->flags & SNDRV_PCM_SYNC_PTR_APPL) s->control->appl_ptr = sp->c.control.appl_ptr;
      if (sp->flags & SNDRV_PCM_SYNC_PTR_AVAIL_MIN) s->control->avail_min = sp->c.control.avail_min;
    }
  }
  // return our status/control views
  if (s->status) sp->s.status = *s->status;
  if (s->control) sp->c.control = *s->control;
  return 0;
}

// RW access: WRITEI copies guest bytes into the ring buffer and advances appl_ptr.
static long pcm_writei(snd_stream* s, struct snd_xferi* x) {
  if (s->dir != SNDRV_PCM_STREAM_PLAYBACK) return -LX_EINVAL;
  snd_pcm_uframes_t av = avail_playback(s);
  snd_pcm_uframes_t n = x->frames < av ? x->frames : av;
  snd_pcm_uframes_t ap = s->control ? s->control->appl_ptr : 0;
  uint8_t* dst = s->data;
  const uint8_t* src = x->buf;
  for (snd_pcm_uframes_t i = 0; i < n; i++) {
    snd_pcm_uframes_t idx = (ap + i) % s->buffer_size;
    memcpy(dst + idx * s->frame_bytes, src + i * s->frame_bytes, s->frame_bytes);
  }
  if (s->control) s->control->appl_ptr = (ap + n) % s->boundary;
  if (n == 0 && x->frames > 0 && s->state != SNDRV_PCM_STATE_RUNNING)
    s->state = SNDRV_PCM_STATE_RUNNING;   // nothing buffered yet; let the clock consume
  x->result = (snd_pcm_sframes_t)n;
  return 0;
}

static long pcm_readi(snd_stream* s, struct snd_xferi* x) {
  if (s->dir != SNDRV_PCM_STREAM_CAPTURE) return -LX_EINVAL;
  // capture returns silence; advance hw_ptr as if captured.
  snd_pcm_uframes_t av = avail_capture(s);
  snd_pcm_uframes_t n = x->frames < av ? x->frames : av;
  snd_pcm_uframes_t hw = s->status ? s->status->hw_ptr : 0;
  uint8_t* dst = x->buf;
  if (dst) for (snd_pcm_uframes_t i = 0; i < n; i++)
    memset(dst + i * s->frame_bytes, 0, s->frame_bytes);
  if (s->status) s->status->hw_ptr = (hw + n) % s->boundary;
  x->result = (snd_pcm_sframes_t)n;
  return 0;
}

static long pcm_ioctl(snd_stream* s, int fd, unsigned nr, unsigned long arg) {
  switch (nr) {
    case NR_PCM_PVERSION:       if (arg) *(int*)arg = SNDRV_PCM_VERSION; return 0;
    case NR_PCM_USER_PVERSION:  return 0;
    case NR_PCM_INFO:           if (arg) fill_pcm_info((struct snd_pcm_info*)arg, s->dir); return 0;
    case NR_PCM_TSTAMP:         return 0;
    case NR_PCM_TTSTAMP:        return 0;
    case NR_PCM_HW_REFINE:      return arg ? pcm_hw_refine((struct snd_pcm_hw_params*)arg) : -LX_EFAULT;
    case NR_PCM_HW_PARAMS:      return arg ? pcm_hw_params(s, (struct snd_pcm_hw_params*)arg) : -LX_EFAULT;
    case NR_PCM_HW_FREE:        stream_free(s); ca_playback_stop(); return 0;
    case NR_PCM_SW_PARAMS:      return arg ? pcm_sw_params(s, (struct snd_pcm_sw_params*)arg) : -LX_EFAULT;
    case NR_PCM_PREPARE:        return pcm_prepare(s);
    case NR_PCM_RESET:          stream_zero_counters(s); s->state = SNDRV_PCM_STATE_SETUP; return 0;
    case NR_PCM_START:
      if (s->status) s->status->state = SNDRV_PCM_STATE_RUNNING;
      s->state = SNDRV_PCM_STATE_RUNNING;
      snd_playback_notify();
      if (tracing()) fprintf(stderr, "[snd] START dir=%d\n", s->dir);
      return 0;
    case NR_PCM_DROP:
      if (s->status) s->status->state = SNDRV_PCM_STATE_SETUP;
      s->state = SNDRV_PCM_STATE_SETUP;
      if (s->control) s->control->appl_ptr = 0;
      if (s->status) s->status->hw_ptr = 0;
      if (s->dir == SNDRV_PCM_STREAM_PLAYBACK) ca_playback_stop();
      return 0;
    case NR_PCM_DRAIN:
      // Playback drain: report DRAINING; CoreAudio keeps pulling until empty then we go to SETUP.
      if (s->dir == SNDRV_PCM_STREAM_PLAYBACK && s->status) s->status->state = SNDRV_PCM_STATE_DRAINING;
      s->state = SNDRV_PCM_STATE_DRAINING;
      return 0;
    case NR_PCM_PAUSE:
      if (s->status) s->status->state = SNDRV_PCM_STATE_PAUSED;
      s->state = SNDRV_PCM_STATE_PAUSED;
      return 0;
    case NR_PCM_RESUME:
      if (s->status) s->status->state = SNDRV_PCM_STATE_RUNNING;
      s->state = SNDRV_PCM_STATE_RUNNING;
      return 0;
    case NR_PCM_XRUN:
      if (s->status) s->status->state = SNDRV_PCM_STATE_XRUN;
      s->state = SNDRV_PCM_STATE_XRUN;
      return 0;
    case NR_PCM_STATUS:
    case NR_PCM_STATUS_EXT:
      return arg ? pcm_status(s, (struct snd_pcm_status*)arg) : -LX_EFAULT;
    case NR_PCM_DELAY:          if (arg) *(snd_pcm_sframes_t*)arg = 0; return 0;
    case NR_PCM_HWSYNC:         return 0;
    case NR_PCM_SYNC_PTR:       return arg ? pcm_sync_ptr(s, (struct snd_pcm_sync_ptr*)arg) : -LX_EFAULT;
    case NR_PCM_CHANNEL_INFO: {
      if (!arg) return -LX_EFAULT;
      struct snd_pcm_channel_info* ci = (struct snd_pcm_channel_info*)arg;
      ci->offset = 0;
      ci->first = ci->channel * (unsigned)(sample_bytes(s->format) * 8);
      ci->step = (unsigned)(s->frame_bytes * 8);
      return 0;
    }
    case NR_PCM_WRITEI_FRAMES:  return arg ? pcm_writei(s, (struct snd_xferi*)arg) : -LX_EFAULT;
    case NR_PCM_READI_FRAMES:   return arg ? pcm_readi(s, (struct snd_xferi*)arg) : -LX_EFAULT;
    case NR_PCM_REWIND:         return 0;
    case NR_PCM_FORWARD:        return 0;
    case NR_PCM_LINK:
    case NR_PCM_UNLINK:         return 0;
    default:
      if (tracing()) fprintf(stderr, "[snd] unhandled PCM ioctl fd=%d nr=0x%02x\n", fd, nr);
      return -LX_ENOTTY;
  }
}

// ---- control ioctl handlers ------------------------------------------------------------
static long ctl_ioctl(int fd, unsigned nr, unsigned long arg) {
  switch (nr) {
    case NR_CTL_PVERSION:       if (arg) *(int*)arg = SNDRV_CTL_VERSION; return 0;
    case NR_CTL_CARD_INFO: {
      if (!arg) return -LX_EFAULT;
      struct snd_ctl_card_info* c = (struct snd_ctl_card_info*)arg;
      memset(c, 0, sizeof *c);
      c->card = 0;
      memcpy(c->id, "rdmetal", 8);
      memcpy(c->driver, "rdmetal", 8);
      memcpy(c->name, "rdmetal", 8);
      memcpy(c->longname, "rdmetal virtual audio (shim CoreAudio)", 41);
      memcpy(c->mixername, "rdmetal", 8);
      memcpy(c->components, "rdmetal", 8);
      return 0;
    }
    case NR_CTL_PCM_NEXT_DEVICE: {
      if (!arg) return -LX_EFAULT;
      int* dev = (int*)arg;
      if (*dev < 0) { *dev = 0; return 0; }
      if (*dev == 0) { *dev = 1; return 0; }   // one playback device (0); capture is a separate substream
      return -LX_ENODEV;
    }
    case NR_CTL_PCM_INFO: {
      if (!arg) return -LX_EFAULT;
      struct snd_pcm_info* info = (struct snd_pcm_info*)arg;
      // stream field tells which direction the caller is asking about
      fill_pcm_info(info, info->stream);
      return 0;
    }
    case NR_CTL_PCM_PREFER_SUBDEVICE: return 0;
    case NR_CTL_HWDEP_NEXT_DEVICE:   return -LX_ENODEV;
    case NR_CTL_RAWMIDI_NEXT_DEVICE: return -LX_ENODEV;
    case NR_CTL_ELEM_LIST: {
      if (!arg) return -LX_EFAULT;
      struct snd_ctl_elem_list* l = (struct snd_ctl_elem_list*)arg;
      l->used = 0;
      l->count = 0;   // no mixer elements
      return 0;
    }
    case NR_CTL_ELEM_INFO: return -LX_ENODEV;
    case NR_CTL_ELEM_READ:
    case NR_CTL_ELEM_WRITE: return -LX_ENODEV;
    case NR_CTL_SUBSCRIBE_EVENTS: return 0;
    case NR_CTL_POWER:          return 0;
    case NR_CTL_POWER_STATE:    if (arg) *(int*)arg = SNDRV_CTL_POWER_D0; return 0;
    default:
      if (tracing()) fprintf(stderr, "[snd] unhandled CTL ioctl fd=%d nr=0x%02x\n", fd, nr);
      return -LX_ENOTTY;
  }
}

// ---- top-level dispatch ----------------------------------------------------------------
long snd_ioctl(int fd, unsigned long req, unsigned long arg) {
  unsigned type = IOC_TYPE(req), nr = IOC_NR(req);
  if (tracing()) {
    if (type == SND_TYPE_PCM)
      fprintf(stderr, "[snd] PCM ioctl fd=%d nr=0x%02x arg=%#lx\n", fd, nr, arg);
    else if (type == SND_TYPE_CTL)
      fprintf(stderr, "[snd] CTL ioctl fd=%d nr=0x%02x arg=%#lx\n", fd, nr, arg);
  }
  if (type == SND_TYPE_CTL) return ctl_ioctl(fd, nr, arg);
  if (type == SND_TYPE_PCM) {
    snd_stream* s = stream_by_fd(fd);
    if (!s) return -LX_ENODEV;
    return pcm_ioctl(s, fd, nr, arg);
  }
  return -LX_ENOTTY;
}

// mmap: map the ALSA offset into the status/control/data pages.
void* snd_mmap(int fd, long off) {
  snd_stream* s = stream_by_fd(fd);
  if (!s || !snd_enabled()) return NULL;
  uint32_t u = (uint32_t)off;
  if (u == SNDRV_PCM_MMAP_OFFSET_STATUS) {
    if (!s->status) stream_alloc_pages(s);
    if (!s->status) return NULL;
    return mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, s->status_fd, 0);
  }
  if (u == SNDRV_PCM_MMAP_OFFSET_CONTROL) {
    if (!s->control) stream_alloc_pages(s);
    if (!s->control) return NULL;
    return mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, s->control_fd, 0);
  }
  if (u == SNDRV_PCM_MMAP_OFFSET_DATA) {
    if (!s->data) return NULL;
    return mmap(NULL, s->data_len, PROT_READ | PROT_WRITE, MAP_SHARED, s->data_fd, 0);
  }
  return NULL;
}

// ---- poll support ----------------------------------------------------------------------
// libasound's snd_pcm_wait() polls the PCM fd (POLLOUT for playback space, POLLIN for capture
// data). Return the Linux revents bits that are ready right now; net.c merges this with its
// host poll in a bounded slice loop.
int snd_poll_revents(int fd, short events) {
  snd_stream* s = stream_by_fd(fd);
  if (!s) return 0;
  int rev = 0;
  if (s->dir == SNDRV_PCM_STREAM_PLAYBACK) {
    if ((events & 0x0004 /*L_POLLOUT*/) && avail_playback(s) >= s->avail_min) rev |= 0x0004;
    if (s->state == SNDRV_PCM_STATE_XRUN) rev |= 0x0004;   // always writable after xrun
  } else {
    if ((events & 0x0001 /*L_POLLIN*/) && avail_capture(s) > 0) rev |= 0x0001;
  }
  if (s->state == SNDRV_PCM_STATE_DRAINING && s->dir == SNDRV_PCM_STREAM_PLAYBACK) rev |= 0x0004;
  return rev;
}

// ---- CoreAudio bridge ------------------------------------------------------------------
unsigned snd_playback_rate(void)     { return g_play.in_use ? g_play.rate : 0; }
unsigned snd_playback_channels(void) { return g_play.in_use ? g_play.channels : 0; }

static float sample_to_float(const void* p, uint32_t fmt) {
  if (fmt == SNDRV_PCM_FORMAT_S16_LE) { int16_t v; memcpy(&v, p, 2); return v / 32768.0f; }
  if (fmt == SNDRV_PCM_FORMAT_S32_LE) { int32_t v; memcpy(&v, p, 4); return v / 2147483648.0f; }
  float v; memcpy(&v, p, 4); return v;   // FLOAT_LE
}

// Consume frames from the playback ring buffer. Returns frames actually consumed.
unsigned snd_playback_pull(float* dst, unsigned nframes) {
  snd_stream* s = &g_play;
  if (!s->in_use || !s->prepared || !s->data) return 0;
  int st = s->status ? s->status->state : s->state;
  if (st != SNDRV_PCM_STATE_RUNNING && st != SNDRV_PCM_STATE_DRAINING) return 0;

  snd_pcm_uframes_t hw = __atomic_load_n(&s->status->hw_ptr, __ATOMIC_ACQUIRE);
  snd_pcm_uframes_t ap = __atomic_load_n(&s->control->appl_ptr, __ATOMIC_ACQUIRE);
  snd_pcm_uframes_t boundary = s->boundary ? s->boundary : s->buffer_size;
  snd_pcm_uframes_t used = (ap - hw) % boundary;
  if (used > s->buffer_size) used = s->buffer_size;
  snd_pcm_uframes_t n = nframes < used ? nframes : used;

  const uint8_t* base = s->data;
  size_t fbytes = s->frame_bytes;
  uint32_t fmt = s->format;
  for (snd_pcm_uframes_t i = 0; i < n; i++) {
    snd_pcm_uframes_t idx = (hw + i) % s->buffer_size;
    const uint8_t* f = base + idx * fbytes;
    for (uint32_t c = 0; c < s->channels; c++)
      dst[i * s->channels + c] = sample_to_float(f + c * (fbytes / s->channels), fmt);
  }
  snd_pcm_uframes_t nhw = (hw + n) % boundary;
  __atomic_store_n(&s->status->hw_ptr, nhw, __ATOMIC_RELEASE);
  // If we drained everything and the stream is DRAINING, complete the drain.
  if (st == SNDRV_PCM_STATE_DRAINING && n == 0) {
    s->status->state = SNDRV_PCM_STATE_SETUP;
    s->state = SNDRV_PCM_STATE_SETUP;
  }
  return (unsigned)n;
}

// Called when stream state changes; the CoreAudio host starts/stops on demand.
void snd_playback_notify(void) {
  if (tracing()) fprintf(stderr, "[snd] notify state=%d rate=%u ch=%u\n",
                         g_play.state, g_play.rate, g_play.channels);
  if (g_play.in_use && (g_play.state == SNDRV_PCM_STATE_RUNNING ||
                        g_play.state == SNDRV_PCM_STATE_DRAINING))
    ca_playback_start(g_play.rate, g_play.channels);
}
