/* guest-edid.c — verify the rdmetal DRM node serves a synthesized 128-byte EDID.
 *
 * Freestanding aarch64 Linux guest (nostdlib, static PIE, `svc #0` syscalls). It
 * opens /dev/dri/card0, reads the primary connector's mode/property surface via
 * MODE_GETCONNECTOR, finds the "EDID" blob property, and asserts MODE_GETPROPBLOB
 * returns a 128-byte block whose first 8 bytes are the EDID header.
 *
 * On success it writes EDID_OK and exits 0; every failure exits non-zero with a
 * distinct code so the harness can bisect the gap.
 */
#include <stdint.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* ---- raw aarch64 syscalls --------------------------------------------------- */
static long sc3(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}

static long sc4(long nr, long a, long b, long c, long d) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}

#define SYS_openat 56
#define SYS_ioctl  29
#define SYS_write  64
#define SYS_exit   93

#define AT_FDCWD (-100L)
#define O_RDWR 2
#define O_CLOEXEC 0x80000

static void fail(int code) {
  sc3(SYS_exit, code, 0, 0);
  for (;;) {}
}

static void emit_ok(void) {
  static const char ok[] = "EDID_OK\n";
  sc3(SYS_write, 1, (long)ok, sizeof(ok) - 1);
}

/* ---- minimal libc helpers --------------------------------------------------- */
static void memset8(void* p, unsigned char v, unsigned long n) {
  unsigned char* b = (unsigned char*)p;
  for (unsigned long i = 0; i < n; i++) b[i] = v;
}

static int streq(const char* a, const char* b) {
  while (*a && *b && *a == *b) { a++; b++; }
  return *a == *b;
}

/* ---- DRM ioctl ABI (mirrors Linux include/uapi/drm/drm_mode.h) -------------- */
#define _IOC_NRBITS    8
#define _IOC_TYPEBITS  8
#define _IOC_SIZEBITS 14
#define _IOC_DIRBITS   2
#define _IOC_NRSHIFT   0
#define _IOC_TYPESHIFT (_IOC_NRSHIFT + _IOC_NRBITS)
#define _IOC_SIZESHIFT (_IOC_TYPESHIFT + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT  (_IOC_SIZESHIFT + _IOC_SIZEBITS)
#define _IOC_NONE  0U
#define _IOC_WRITE 1U
#define _IOC_READ  2U
#define _IOC(dir, type, nr, size) \
  (((dir) << _IOC_DIRSHIFT) | ((type) << _IOC_TYPESHIFT) | \
   ((nr) << _IOC_NRSHIFT) | ((size) << _IOC_SIZESHIFT))
#define _IOWR(type, nr, size) _IOC(_IOC_READ | _IOC_WRITE, (type), (nr), (size))

#define DRM_IOCTL_BASE 'd'

struct drm_mode_get_connector {
  uint64_t encoders_ptr, modes_ptr, props_ptr, prop_values_ptr;
  uint32_t count_modes, count_props, count_encoders;
  uint32_t encoder_id;
  uint32_t connector_id;
  uint32_t connector_type;
  uint32_t connector_type_id;
  uint32_t connection;
  uint32_t mm_width, mm_height;
  uint32_t subpixel;
  uint32_t pad;
};

struct drm_mode_get_blob {
  uint32_t blob_id, length;
  uint64_t data;
};

struct drm_mode_get_property {
  uint64_t values_ptr, enum_blob_ptr;
  uint32_t prop_id, flags;
  char name[32];
  uint32_t count_values, count_enum_blobs;
};

#define DRM_IOCTL_MODE_GETCONNECTOR \
  _IOWR(DRM_IOCTL_BASE, 0xA7, sizeof(struct drm_mode_get_connector))
#define DRM_IOCTL_MODE_GETPROPBLOB \
  _IOWR(DRM_IOCTL_BASE, 0xAC, sizeof(struct drm_mode_get_blob))
#define DRM_IOCTL_MODE_GETPROPERTY \
  _IOWR(DRM_IOCTL_BASE, 0xAA, sizeof(struct drm_mode_get_property))

void _start(void) {
  int fd = (int)sc4(SYS_openat, AT_FDCWD, (long)"/dev/dri/card0", O_RDWR | O_CLOEXEC, 0);
  if (fd < 0) fail(1);

  /* Two-call GETCONNECTOR: first query returns the counts, second fills the arrays. */
  struct drm_mode_get_connector conn;
  memset8(&conn, 0, sizeof conn);
  conn.connector_id = 1;
  if (sc3(SYS_ioctl, fd, DRM_IOCTL_MODE_GETCONNECTOR, (long)&conn) != 0) fail(2);
  if (conn.count_modes < 1) fail(3);
  if (conn.count_props < 1 || conn.count_props > 8) fail(4);

  uint32_t props[8];
  uint64_t prop_vals[8];
  memset8(props, 0, sizeof props);
  memset8(prop_vals, 0, sizeof prop_vals);
  conn.props_ptr = (uint64_t)(long)props;
  conn.prop_values_ptr = (uint64_t)(long)prop_vals;
  if (sc3(SYS_ioctl, fd, DRM_IOCTL_MODE_GETCONNECTOR, (long)&conn) != 0) fail(5);
  if (conn.count_modes < 1) fail(6);

  /* The Linux UAPI exposes the EDID through the connector's "EDID" blob property
   * (there is no edid_blob_id struct field); resolve its id by name. */
  uint32_t edid_blob_id = 0;
  for (uint32_t i = 0; i < conn.count_props; i++) {
    struct drm_mode_get_property prop;
    memset8(&prop, 0, sizeof prop);
    prop.prop_id = props[i];
    if (sc3(SYS_ioctl, fd, DRM_IOCTL_MODE_GETPROPERTY, (long)&prop) != 0) fail(7);
    if (streq(prop.name, "EDID")) {
      edid_blob_id = (uint32_t)prop_vals[i];
      break;
    }
  }
  if (edid_blob_id == 0) fail(8);

  unsigned char edid[128];
  memset8(edid, 0, sizeof edid);
  struct drm_mode_get_blob blob;
  blob.blob_id = edid_blob_id;
  blob.length = 0;
  blob.data = (uint64_t)(long)edid;
  if (sc3(SYS_ioctl, fd, DRM_IOCTL_MODE_GETPROPBLOB, (long)&blob) != 0) fail(9);
  if (blob.length != 128) fail(10);

  static const unsigned char header[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
  for (int i = 0; i < 8; i++)
    if (edid[i] != header[i]) fail(11);

  emit_ok();
  fail(0);
}
