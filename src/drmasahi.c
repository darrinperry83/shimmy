// drmasahi.c — present a DRM render node so Mesa's ASAHI driver will load.
//
// WHY. Most of an Asahi GPU driver is USERSPACE: the NIR->AGX shader compiler, the Gallium
// driver, the command-buffer builder. All of it is a Linux program talking to the kernel through
// a short list of ioctls, and this shim runs Linux programs. Debian's Mesa already ships the
// driver — `strings libgallium` lists exactly nine entry points:
//
//   GET_PARAMS  VM_CREATE  VM_BIND  GEM_CREATE  GEM_BIND_OBJECT
//   MMAP_BO     QUEUE_CREATE  SUBMIT  GET_TIME
//
// With those answered, Mesa's AGX compiler runs on macOS, next to Apple's Metal compiler, in one
// process — which makes a differential test harness for the AGX backend possible without a
// reboot, without m1n1, and without a second machine.
//
// THIS FILE IS DELIBERATELY A LOGGER FIRST. The asahi UAPI has changed shape more than once and
// guessing struct layouts from memory is how you get a plausible-looking harness that measures
// nothing. So: answer DRM_IOCTL_VERSION honestly (Mesa selects the driver by that name), and
// RECORD every other request — direction, type, number, payload size — then read the real
// sequence off the machine and implement against that.
//
// RDDRM=0     hide the node (present by default: it changes what Mesa does)
// RDDRM_TRACE=1  log every ioctl
#include "drmasahi.h"
#include "rdgpu_surface_handoff.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurface.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#define IOC_NRSHIFT    0
#define IOC_TYPESHIFT  8
#define IOC_SIZESHIFT  16
#define IOC_DIRSHIFT   30
#define IOC_NR(x)   (((x) >> IOC_NRSHIFT)   & 0xFF)
#define IOC_TYPE(x) (((x) >> IOC_TYPESHIFT) & 0xFF)
#define IOC_SIZE(x) (((x) >> IOC_SIZESHIFT) & 0x3FFF)
#define IOC_DIR(x)  (((x) >> IOC_DIRSHIFT)  & 0x3)

#define DRM_TYPE          'd'
#define DRM_COMMAND_BASE  0x40
#define DRM_NR_VERSION    0x00
#define DRM_NR_GEM_CLOSE  0x09
#define DRM_NR_PRIME_HANDLE_TO_FD 0x2D
#define DRM_NR_PRIME_FD_TO_HANDLE 0x2E
#define DRM_NR_MODE_CREATE_DUMB   0xb2
#define DRM_NR_MODE_MAP_DUMB      0xb3
#define DRM_NR_MODE_DESTROY_DUMB  0xb4

// KMS surface aquamarine's DRM backend needs to start (checkFeatures + resources).
// Numbers are the DRM_IOCTL_* "nr" values from <libdrm/drm.h>/<libdrm/drm_mode.h>;
// the full command is (dir<<30)|(0x64<<8)|nr|(sz<<16) where 'd'==0x64.
#define DRM_NR_GET_CAP               0x0c
#define DRM_NR_SET_CLIENT_CAP        0x0d
#define DRM_NR_MODE_GETRESOURCES     0xa0
#define DRM_NR_MODE_GETCRTC          0xa1
#define DRM_NR_MODE_GETENCODER       0xa6
#define DRM_NR_MODE_GETCONNECTOR     0xa7
#define DRM_NR_MODE_GETPROPERTY      0xaa
#define DRM_NR_MODE_GETPROPBLOB      0xac
#define DRM_NR_MODE_RMFB             0xaf
#define DRM_NR_MODE_GETPLANERESOURCES 0xb5
#define DRM_NR_MODE_GETPLANE         0xb6
#define DRM_NR_MODE_ADDFB2           0xb8
#define DRM_NR_MODE_OBJ_GETPROPERTIES 0xb9

// DRM capabilities (drm.h). TIMESTAMP_MONOTONIC is 0x6, ASYNC_PAGE_FLIP is 0x7,
// CRTC_IN_VBLANK_EVENT is 0x12 (the "0x6 vs 0x7" confusion in the brief is these three).
#define DRM_CAP_DUMB_BUFFER          0x1
#define DRM_CAP_PRIME                0x5
#define DRM_CAP_TIMESTAMP_MONOTONIC  0x6
#define DRM_CAP_ASYNC_PAGE_FLIP      0x7
#define DRM_CAP_CURSOR_WIDTH         0x8
#define DRM_CAP_CURSOR_HEIGHT        0x9
#define DRM_CAP_ADDFB2_MODIFIERS     0x10
#define DRM_CAP_CRTC_IN_VBLANK_EVENT 0x12
#define DRM_CAP_SYNCOBJ              0x13
#define DRM_CAP_SYNCOBJ_TIMELINE     0x14
#define DRM_PRIME_CAP_IMPORT         0x1
#define DRM_PRIME_CAP_EXPORT         0x2

#define DRM_CLIENT_CAP_UNIVERSAL_PLANES 2
#define DRM_CLIENT_CAP_ATOMIC           3

// Connector/encoder/mode constants used to answer the KMS probes plausibly.
#define DRM_MODE_CONNECTOR_eDP    14
#define DRM_MODE_ENCODER_VIRTUAL  5
#define DRM_MODE_CONNECTED        1
#define DRM_MODE_TYPE_PREFERRED   8

// Property surface (drm_mode.h). The plane exposes one enum property "type"
// valued DRM_PLANE_TYPE_PRIMARY; CRTC/connector expose none.
#define DRM_MODE_OBJECT_CRTC      0xcccccccc
#define DRM_MODE_OBJECT_CONNECTOR 0xc0c0c0c0
#define DRM_MODE_OBJECT_PLANE     0xeeeeeeee
#define DRM_MODE_OBJECT_ANY       0
#define DRM_MODE_PROP_IMMUTABLE   (1 << 2)
#define DRM_MODE_PROP_ENUM        (1 << 3)
#define DRM_MODE_PROP_ATOMIC      0x80000000
#define DRM_PLANE_TYPE_PRIMARY    1
#define DRM_FORMAT_XRGB8888       0x34325258   // fourcc 'XR24'

// struct drm_version, 64-bit layout (offsets verified against drm.h):
//   0 version_major(i32) 4 version_minor(i32) 8 version_patchlevel(i32) 12 pad
//  16 name_len(size_t) 24 name(char*) 32 date_len 40 date 48 desc_len 56 desc   => 64 bytes
struct drm_version_64 {
  int32_t major, minor, patchlevel; int32_t _pad;
  uint64_t name_len; char* name;
  uint64_t date_len; char* date;
  uint64_t desc_len; char* desc;
};

// struct drm_prime_handle (12 bytes) and struct drm_gem_close (8 bytes), per the Linux UAPI.
struct drm_prime_handle { uint32_t handle; uint32_t flags; int32_t fd; };
struct drm_gem_close    { uint32_t handle; uint32_t pad; };

// --- KMS structs, 64-bit layouts (offsets verified against libdrm drm.h/drm_mode.h). ---
struct drm_get_cap        { uint64_t capability; uint64_t value; };                 // 16
struct drm_set_client_cap { uint64_t capability; uint64_t value; };                 // 16

struct drm_mode_card_res {
  uint64_t fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr;                // 32
  uint32_t count_fbs, count_crtcs, count_connectors, count_encoders;                // 16 -> 48
  uint32_t min_width, max_width, min_height, max_height;                            // 16 -> 64
};

struct drm_mode_modeinfo {
  uint32_t clock;
  uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
  uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
  uint32_t vrefresh;
  uint32_t flags;
  uint32_t type;
  char name[32];                                                                     // 68 total
};

// kernel name: struct drm_mode_crtc. set_connectors_ptr must precede the scalars.
struct drm_mode_get_crtc {
  uint64_t set_connectors_ptr;
  uint32_t count_connectors;
  uint32_t crtc_id;              // input (caller sets); rest is output
  uint32_t fb_id;
  uint32_t x, y;
  uint32_t gamma_size;
  uint32_t mode_valid;
  struct drm_mode_modeinfo mode;                                                     // 104 total
};

struct drm_mode_get_encoder {
  uint32_t encoder_id;           // input
  uint32_t encoder_type;
  uint32_t crtc_id;
  uint32_t possible_crtcs;
  uint32_t possible_clones;                                                          // 20 total
};

struct drm_mode_get_connector {
  uint64_t encoders_ptr, modes_ptr, props_ptr, prop_values_ptr;                      // 32
  uint32_t count_modes, count_props, count_encoders;                                 // 12 -> 44
  uint32_t encoder_id;
  uint32_t connector_id;         // input
  uint32_t connector_type;
  uint32_t connector_type_id;
  uint32_t connection;
  uint32_t mm_width, mm_height;
  uint32_t subpixel;
  uint32_t pad;                                                                      // 80 total
};

struct drm_mode_get_plane_res {
  uint64_t plane_id_ptr;
  uint32_t count_planes;                                                             // 16 total
};

struct drm_mode_fb_cmd2 {
  uint32_t fb_id, width, height, pixel_format, flags;                                // 20
  uint32_t handles[4];                                                               // 16 -> 36
  uint32_t pitches[4];                                                               // 16 -> 52
  uint32_t offsets[4];                                                               // 16 -> 68
  uint64_t modifier[4];                                                              // 32 -> 104 (4 pad)
};

struct drm_mode_obj_get_properties {
  uint64_t props_ptr, prop_values_ptr;                                               // 16
  uint32_t count_props, obj_id, obj_type;                                            // 12 -> 32 (4 pad)
};

struct drm_mode_get_property {
  uint64_t values_ptr, enum_blob_ptr;                                                // 16
  uint32_t prop_id, flags;                                                           // 8 -> 24
  char name[32];                                                                     // 32 -> 56
  uint32_t count_values, count_enum_blobs;                                           // 8 -> 64
};

struct drm_mode_get_blob {
  uint32_t blob_id, length;                                                          // 8
  uint64_t data;                                                                     // 8 -> 16
};

struct drm_mode_get_plane {
  uint32_t plane_id, crtc_id, fb_id;                                                 // 12
  uint32_t possible_crtcs, gamma_size;                                               // 8 -> 20
  uint32_t count_format_types;                                                       // 4 -> 24
  uint64_t format_type_ptr;                                                          // 8 -> 32
};

// Dumb-buffer ioctls (drm_mode.h). MODE_CREATE_DUMB allocates a linear CPU-mapped
// framebuffer; MODE_MAP_DUMB returns its fake mmap offset (we reuse the GEM scheme
// offset = handle << 12); MODE_DESTROY_DUMB releases it. Used by the sw winsys
// (kms_sw) so softpipe render targets are the SAME host backing the present path reads.
struct drm_mode_create_dumb {
  uint32_t height, width, bpp, flags;                                                // 16
  uint32_t handle, pitch;                                                            // 8 -> 24
  uint64_t size;                                                                     // 8 -> 32
};
struct drm_mode_map_dumb {
  uint32_t handle, pad;                                                              // 8
  uint64_t offset;                                                                   // 8 -> 16
};
struct drm_mode_destroy_dumb {
  uint32_t handle;                                                                   // 4
};

// Compile-time guard: if a layout drifts from the Linux UAPI, the build fails
// instead of silently answering ioctls with a mis-sized struct.
_Static_assert(sizeof(struct drm_get_cap)         == 16,  "drm_get_cap");
_Static_assert(sizeof(struct drm_set_client_cap)  == 16,  "drm_set_client_cap");
_Static_assert(sizeof(struct drm_mode_card_res)   == 64,  "drm_mode_card_res");
_Static_assert(sizeof(struct drm_mode_modeinfo)   == 68,  "drm_mode_modeinfo");
_Static_assert(sizeof(struct drm_mode_get_crtc)   == 104, "drm_mode_get_crtc");
_Static_assert(sizeof(struct drm_mode_get_encoder) == 20, "drm_mode_get_encoder");
_Static_assert(sizeof(struct drm_mode_get_connector) == 80, "drm_mode_get_connector");
_Static_assert(sizeof(struct drm_mode_get_plane_res) == 16, "drm_mode_get_plane_res");
_Static_assert(sizeof(struct drm_mode_fb_cmd2)    == 104, "drm_mode_fb_cmd2");
_Static_assert(sizeof(struct drm_mode_obj_get_properties) == 32, "drm_mode_obj_get_properties");
_Static_assert(sizeof(struct drm_mode_get_property) == 64, "drm_mode_get_property");
_Static_assert(sizeof(struct drm_mode_get_blob)   == 16,  "drm_mode_get_blob");
_Static_assert(sizeof(struct drm_mode_get_plane)  == 32,  "drm_mode_get_plane");
_Static_assert(sizeof(struct drm_mode_create_dumb) == 32, "drm_mode_create_dumb");
_Static_assert(sizeof(struct drm_mode_map_dumb)    == 16, "drm_mode_map_dumb");
_Static_assert(sizeof(struct drm_mode_destroy_dumb) == 4, "drm_mode_destroy_dumb");

// ---- Asahi UAPI (matches rootfs-arch/usr/include/drm/asahi_drm.h, NEW upstream UAPI) ----
// The Arch Mesa 26.2.6 asahi binary issues nrs = DRM_COMMAND_BASE(0x40) + id. RDDRM_TRACE=1 is the
// ground truth for the numbers; these constants map that trace back to the header struct layouts.
#define ASAHI_NR_GET_PARAMS       0x40   // id 0x00  IOW  24
#define ASAHI_NR_GET_TIME         0x41   // id 0x01  IOWR 16
#define ASAHI_NR_VM_CREATE        0x42   // id 0x02  IOWR 24
#define ASAHI_NR_VM_DESTROY       0x43   // id 0x03  IOW   8
#define ASAHI_NR_VM_BIND          0x44   // id 0x04  IOW  24
#define ASAHI_NR_GEM_CREATE       0x45   // id 0x05  IOWR 24
#define ASAHI_NR_GEM_MMAP_OFFSET  0x46   // id 0x06  IOWR 16
#define ASAHI_NR_GEM_BIND_OBJECT  0x47   // id 0x07  IOWR 40
#define ASAHI_NR_QUEUE_CREATE     0x48   // id 0x08  IOWR 24
#define ASAHI_NR_QUEUE_DESTROY    0x49   // id 0x09  IOW   8
#define ASAHI_NR_SUBMIT           0x4a   // id 0x0a  IOW  40

#define DRM_ASAHI_MAX_CLUSTERS 64

struct asahi_params_global {
  uint64_t features;
  uint32_t gpu_generation;
  uint32_t gpu_variant;
  uint32_t gpu_revision;
  uint32_t chip_id;
  uint32_t num_dies;
  uint32_t num_clusters_total;
  uint32_t num_cores_per_cluster;
  uint32_t max_frequency_khz;
  uint64_t core_masks[DRM_ASAHI_MAX_CLUSTERS];
  uint64_t vm_start;
  uint64_t vm_end;
  uint64_t vm_kernel_min_size;
  uint32_t max_commands_per_submission;
  uint32_t max_attachments;
  uint64_t command_timestamp_frequency_hz;
};
struct asahi_get_params      { uint32_t param_group; uint32_t pad; uint64_t pointer; uint64_t size; };
struct asahi_get_time        { uint64_t flags; uint64_t gpu_timestamp; };
struct asahi_vm_create       { uint64_t kernel_start; uint64_t kernel_end; uint32_t vm_id; uint32_t pad; };
struct asahi_vm_destroy      { uint32_t vm_id; uint32_t pad; };
struct asahi_gem_bind_op     { uint32_t flags; uint32_t handle; uint64_t offset; uint64_t range; uint64_t addr; };
struct asahi_vm_bind         { uint32_t vm_id; uint32_t num_binds; uint32_t stride; uint32_t pad; uint64_t userptr; };
struct asahi_gem_create      { uint64_t size; uint32_t flags; uint32_t vm_id; uint32_t handle; uint32_t pad; };
struct asahi_gem_mmap_offset { uint32_t handle; uint32_t flags; uint64_t offset; };
struct asahi_gem_bind_object { uint32_t op; uint32_t flags; uint32_t handle; uint32_t vm_id;
                               uint64_t offset; uint64_t range; uint32_t object_handle; uint32_t pad; };
struct asahi_queue_create    { uint32_t flags; uint32_t vm_id; uint32_t priority; uint32_t queue_id;
                               uint64_t usc_exec_base; };
struct asahi_queue_destroy   { uint32_t queue_id; uint32_t pad; };
struct asahi_sync            { uint32_t sync_type; uint32_t handle; uint64_t timeline_value; };
struct asahi_submit          { uint64_t syncs; uint64_t cmdbuf; uint32_t flags; uint32_t queue_id;
                               uint32_t in_sync_count; uint32_t out_sync_count;
                               uint32_t cmdbuf_size; uint32_t pad; };

_Static_assert(sizeof(struct asahi_params_global) == 592, "asahi_params_global");
_Static_assert(sizeof(struct asahi_get_params)      == 24,  "asahi_get_params");
_Static_assert(sizeof(struct asahi_get_time)        == 16,  "asahi_get_time");
_Static_assert(sizeof(struct asahi_vm_create)       == 24,  "asahi_vm_create");
_Static_assert(sizeof(struct asahi_vm_destroy)      == 8,   "asahi_vm_destroy");
_Static_assert(sizeof(struct asahi_gem_bind_op)     == 32,  "asahi_gem_bind_op");
_Static_assert(sizeof(struct asahi_vm_bind)         == 24,  "asahi_vm_bind");
_Static_assert(sizeof(struct asahi_gem_create)      == 24,  "asahi_gem_create");
_Static_assert(sizeof(struct asahi_gem_mmap_offset) == 16,  "asahi_gem_mmap_offset");
_Static_assert(sizeof(struct asahi_gem_bind_object) == 40,  "asahi_gem_bind_object");
_Static_assert(sizeof(struct asahi_queue_create)    == 24,  "asahi_queue_create");
_Static_assert(sizeof(struct asahi_queue_destroy)   == 8,   "asahi_queue_destroy");
_Static_assert(sizeof(struct asahi_sync)            == 16,  "asahi_sync");
_Static_assert(sizeof(struct asahi_submit)          == 40,  "asahi_submit");

// OLD "full UAPI" params_global (Asahi Lina 10008 layout, 32 clusters) — the closest known
// reference for the OLD UAPI the binary was built against. We VERIFY against the driver's
// requested size (p->size) and its subsequent VM_CREATE args, and adjust if needed.
struct asahi_params_global_old {
  uint32_t unstable_uabi_version;
  uint32_t pad0;
  uint64_t feat_compat;
  uint64_t feat_incompat;
  uint32_t gpu_generation;
  uint32_t gpu_variant;
  uint32_t gpu_revision;
  uint32_t chip_id;
  uint32_t num_dies;
  uint32_t num_clusters_total;
  uint32_t num_cores_per_cluster;
  uint32_t num_frags_per_cluster;
  uint32_t num_gps_per_cluster;
  uint32_t num_cores_total_active;
  uint64_t core_masks[32];
  uint32_t vm_page_size;
  uint32_t pad1;
  uint64_t vm_user_start;
  uint64_t vm_user_end;
  uint64_t vm_shader_start;
  uint64_t vm_shader_end;
  uint32_t max_syncs_per_submission;
  uint32_t max_commands_per_submission;
  uint32_t max_commands_in_flight;
  uint32_t max_attachments;
  uint32_t timer_frequency_hz;
  uint32_t min_frequency_khz;
  uint32_t max_frequency_khz;
  uint32_t max_power_mw;
  uint32_t result_render_size;
  uint32_t result_compute_size;
};
_Static_assert(sizeof(struct asahi_params_global_old) == 400, "asahi_params_global_old");

static int g_on = -1, g_trace = -1;
static int tracing(void);
#define MAXFD 64
static int g_fds[MAXFD];
static int g_nfd;

// GEM slot: a synthetic DRM handle bound to the imported dma-buf carrier's
// fstat(st_dev, st_ino) identity and, once resolved, to the backing IOSurface.
// The importing process stores its own CFRetained reference here; GEM_CLOSE (or
// process teardown) releases it. Resolution goes through
// rdgpu_handoff_resolve_fd() (mach-port handoff: IOSurfaceLookupFromMachPort).
#define MAX_GEM 64
struct gem_slot {
  uint32_t handle;
  int fd;                 // carrier fd at import time (identity only; not owned)
  dev_t device;
  ino_t inode;
  IOSurfaceRef surface;   // CFRetained, NULL until resolved
  int resolved;
};
static struct gem_slot g_gem[MAX_GEM];
static uint32_t g_next_fb = 1;   // framebuffer ids minted by MODE_ADDFB2

static struct gem_slot* gem_find_by_handle(uint32_t handle) {
  for (int i = 0; i < MAX_GEM; i++)
    if (g_gem[i].resolved && g_gem[i].handle == handle) return &g_gem[i];
  return NULL;
}
static struct gem_slot* gem_find_by_identity(dev_t device, ino_t inode) {
  for (int i = 0; i < MAX_GEM; i++)
    if (g_gem[i].resolved && g_gem[i].device == device && g_gem[i].inode == inode)
      return &g_gem[i];
  return NULL;
}
static void gem_release_slot(struct gem_slot* g) {
  if (!g) return;
  if (g->surface) { CFRelease(g->surface); g->surface = NULL; }
  /* Retire the import-side handoff entry too, so a bounded frame pool can
   * run more than RDGPU_HANDOFF_MAX_SURFACES frames. */
  if (g->device && g->inode) rdgpu_handoff_revoke(g->device, g->inode);
  memset(g, 0, sizeof *g);
}

// ---- GEM objects CREATED by the guest (Asahi GEM_CREATE), with CPU backing ----
// The asahi driver allocates BOs (command buffers, shader binaries, scratch, timestamps),
// then mmap()s them via GEM_MMAP_OFFSET. The backing is plain host memory, which the guest
// reads/writes directly (the shim runs the guest in-process), so this is a straight host
// allocation per BO. Handles live in a separate namespace from the imported-surface slots.
#define MAX_BO 512
struct bo_slot {
  uint32_t handle;
  void*    map;      // canonical host view of the backing, page-aligned (present path reads this)
  int      backing_fd; // anonymous backing file the map and every guest mmap alias (MAP_SHARED)
  size_t   size;     // requested size (bytes)
  int      used;
  uint32_t refs;     // GEM-style reference count (creator + importers)
};
static struct bo_slot g_bo[MAX_BO];
static uint32_t g_next_bo_handle = 0x40000000u;   // far from imported-surface handles (1..N)

static struct bo_slot* bo_find(uint32_t handle) {
  for (int i = 0; i < MAX_BO; i++)
    if (g_bo[i].used && g_bo[i].handle == handle) return &g_bo[i];
  return NULL;
}

/* Allocate an anonymous backing file (unlinked mkstemp) sized to rlen and map it
 * MAP_SHARED. The returned pointer is the BO's CANONICAL view (read by the present
 * path via drm_bo_map_by_fd); *fd_out is the fd used to mint fresh MAP_SHARED guest
 * aliases on every guest mmap. This fixes the dumb-buffer lifetime bug: the sw winsys
 * (kms-dri) mmap()s a dumb buffer to write pixels and munmap()s it when its refcount
 * drops to zero. Handing the guest b->map directly meant that munmap tore down the
 * BO's own backing and a later re-mmap returned the same now-unmapped pointer. With a
 * file backing, a guest munmap only releases that alias while the canonical view and
 * the bytes stay live, and each re-mmap is a fresh, valid mapping. */
static void* bo_backing_map(size_t rlen, int* fd_out) {
  *fd_out = -1;
  char tmpl[] = "/tmp/rdmetal-bo-XXXXXX";
  int fd = mkstemp(tmpl);
  if (fd < 0) return MAP_FAILED;
  unlink(tmpl);
  if (ftruncate(fd, (off_t)rlen)) { close(fd); return MAP_FAILED; }
  void* m = mmap(NULL, rlen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (m == MAP_FAILED) { close(fd); return MAP_FAILED; }
  *fd_out = fd;
  return m;
}

/* Increment a BO's reference count. An import (PRIME_FD_TO_HANDLE "own export"
 * round-trip, or a fresh allocation) takes a reference; GEM_CLOSE and
 * MODE_DESTROY_DUMB drop one. The backing is freed only when the count reaches
 * zero, so the compositor's GEM_CLOSE on a re-imported fd never frees a BO the
 * client (GBM) still owns. */
static void bo_ref(struct bo_slot* b) { if (b) b->refs++; }

static void bo_unref(struct bo_slot* b) {
  if (!b) return;
  if (b->refs) b->refs--;
  if (b->refs) return;
  if (b->map) munmap(b->map, (b->size + 4095) & ~(size_t)4095);
  if (b->backing_fd >= 0) close(b->backing_fd);
  memset(b, 0, sizeof *b);
}

// ---- Export side: PRIME_HANDLE_TO_FD carrier fd -> BO mapping ----
// When Hyprland's GBM calls gbm_bo_get_fd_for_plane, PRIME_HANDLE_TO_FD hands back a virtual
// dma-buf carrier fd (an unlinked temp file carrying identity only, same shape as
// rdgpu_virtual_carrier.c). Its fstat(st_dev, st_ino) is recorded here keyed to the BO handle, so
// the compositor's own import (PRIME_FD_TO_HANDLE) and the pixel readback (drm_bo_map_by_fd) can
// find the SAME host backing the asahi driver mmap()ed via GEM_MMAP_OFFSET. The BO's bytes ARE the
// rendered frame (the shim runs the guest in-process), so no cross-process mach-port handoff is
// needed on this path.
#define MAX_EXPORT 128
struct export_slot {
  uint32_t handle;
  dev_t device;
  ino_t inode;
  int used;
};
static struct export_slot g_export[MAX_EXPORT];
static unsigned g_export_next;

static struct export_slot* export_find(dev_t device, ino_t inode) {
  for (int i = 0; i < MAX_EXPORT; i++)
    if (g_export[i].used && g_export[i].device == device && g_export[i].inode == inode)
      return &g_export[i];
  return NULL;
}

static void export_remember(uint32_t handle, dev_t device, ino_t inode) {
  if (!device || !inode) return;
  for (int i = 0; i < MAX_EXPORT; i++)
    if (!g_export[i].used) {
      g_export[i].used = 1;
      g_export[i].handle = handle;
      g_export[i].device = device;
      g_export[i].inode = inode;
      return;
    }
  // Table full: evict the oldest round-robin slot. A stale identity is harmless once the
  // compositor has closed its fd (the inode is gone, so the entry can never be re-matched).
  g_export[g_export_next].used = 1;
  g_export[g_export_next].handle = handle;
  g_export[g_export_next].device = device;
  g_export[g_export_next].inode = inode;
  g_export_next = (g_export_next + 1) % MAX_EXPORT;
}

static void export_forget(uint32_t handle) {
  for (int i = 0; i < MAX_EXPORT; i++)
    if (g_export[i].used && g_export[i].handle == handle) g_export[i].used = 0;
}

// mmap() on a DRM fd (syscall 222) resolves the GEM fake offset to a FRESH MAP_SHARED
// alias of the BO's backing file. Returns NULL if the fd/offset does not name one of our
// BOs. The guest owns the returned mapping (it munmap()s it via the sw winsys), so it must
// be a fresh mapping each call — never the canonical b->map — or a guest munmap tears down
// the BO backing and a re-mmap hands back a dangling pointer.
void* drm_mmap_bo(int fd, long off) {
  (void)fd;
  if (!drm_enabled()) return NULL;
  uint64_t u = (uint64_t)off;
  uint32_t handle = (uint32_t)(u >> 12);           // offset = handle * 4096
  struct bo_slot* b = bo_find(handle);
  if (!b || b->backing_fd < 0) return NULL;
  size_t rlen = (b->size + 4095) & ~(size_t)4095;
  if (rlen == 0) rlen = 4096;
  void* m = mmap(NULL, rlen, PROT_READ | PROT_WRITE, MAP_SHARED, b->backing_fd, 0);
  if (m == MAP_FAILED) return NULL;
  return (char*)m + (size_t)(u & 0xfff);
}

// Resolve a PRIME_HANDLE_TO_FD carrier fd back to the BO's host backing (and size). Used by the
// compositor (wlsrv.c) to read the rendered frame once the dmabuf arrives via zwp_linux_dmabuf_v1
// create_immed — the fd's fstat(st_dev, st_ino) is the stable identity across the SCM_RIGHTS hop.
void* drm_bo_map_by_fd(int fd, size_t* size) {
  if (size) *size = 0;
  if (fd < 0 || !drm_enabled()) return NULL;
  struct stat st;
  if (fstat(fd, &st) || st.st_dev == 0 || st.st_ino == 0) return NULL;
  struct export_slot* e = export_find(st.st_dev, st.st_ino);
  if (!e) return NULL;
  struct bo_slot* b = bo_find(e->handle);
  if (!b) return NULL;
  if (size) *size = b->size;
  return b->map;
}

int drm_enabled(void) {
  // Present by default (the rdmetal render node is now the installed default
  // driver, so Hyprland/KWin must see /dev/dri/renderD128 with no env vars).
  // RDDRM=0 explicitly hides the node; RDDRM=1 is a no-op (already on).
  if (g_on < 0) { const char* e = getenv("RDDRM"); g_on = !(e && *e == '0'); }
  return g_on;
}
static int tracing(void) {
  if (g_trace < 0) { const char* e = getenv("RDDRM_TRACE"); g_trace = (e && *e && *e != '0'); }
  return g_trace;
}

int drm_is_node(const char* p) {
  if (!p || !drm_enabled()) return 0;
  int hit = strcmp(p, "/dev/dri/renderD128") == 0 || strcmp(p, "/dev/dri/card0") == 0;
  // Log every /dev/dri and /sys/dev/char lookup, hit or miss. "Our node was never opened" has
  // two very different causes -- nobody asked, or we asked under a name we do not match -- and
  // only the miss line separates them.
  if (tracing() && (!strncmp(p, "/dev/dri", 8) || !strncmp(p, "/sys/dev/char", 13) ||
                    !strncmp(p, "/sys/class/drm", 14)))
    fprintf(stderr, "[drm] path %-40s %s\n", p, hit ? "MATCH" : "(miss)");
  return hit;
}

void drm_register_fd(int fd) {
  if (fd < 0 || g_nfd >= MAXFD) return;
  g_fds[g_nfd++] = fd;
  if (tracing()) fprintf(stderr, "[drm] opened render node as fd %d\n", fd);
}

void drm_forget_fd(int fd) {
  for (int i = 0; i < g_nfd; i++)
    if (g_fds[i] == fd) { g_fds[i] = g_fds[--g_nfd]; return; }
}

int drm_owns_fd(int fd) {
  for (int i = 0; i < g_nfd; i++) if (g_fds[i] == fd) return 1;
  return 0;
}

// Copy at most *len bytes (Linux semantics: the caller passes capacity, we set the true length).
static void put_str(char* dst, uint64_t* len, const char* s) {
  uint64_t n = (uint64_t)strlen(s);
  if (dst && *len) { uint64_t c = n < *len ? n : *len; memcpy(dst, s, (size_t)c); }
  *len = n;
}

// Hex-dump an ioctl payload so we can reverse-engineer a struct layout the header does not
// describe. Only fires when tracing.
static void trace_hex(const char* label, const void* p, unsigned n) {
  if (!tracing()) return;
  if (!p) { fprintf(stderr, "[drm] %s (arg NULL)\n", label); return; }
  const unsigned char* b = (const unsigned char*)p;
  unsigned cap = n < 96 ? n : 96;
  fprintf(stderr, "[drm] %s (%u bytes):", label, n);
  for (unsigned i = 0; i < cap; i++) {
    if ((i & 15) == 0) fprintf(stderr, "\n  %04x:", i);
    fprintf(stderr, " %02x", b[i]);
  }
  fprintf(stderr, "\n");
}


// libdrm decides a node is a GPU by STATTING it: it must be a character device, and its
// major:minor must be 226:128 (DRM_MAJOR, render node base) so drmGetDevice2() can find the
// matching /sys/dev/char entry. A regular file is rejected before a single ioctl is issued,
// which is why the trace was empty on the first attempt.
#define DRM_MAJOR 226

// ---- sysfs surface for drmGetDeviceNameFromFd2() ----------------------------------------------
//
// aquamarine's CDRMBackend::registerGPU() calls libdrm's drmGetDeviceNameFromFd2(fd), which does
// NOT use a DRM ioctl: it fstat()s the fd for the major:minor, then stat()s
//   /sys/dev/char/<major>:<minor>/device/drm   (must be S_IFCHR)
// and fopen()+reads
//   /sys/dev/char/<major>:<minor>/uevent       (looks for "DEVNAME=...")
// With an empty guest /sys both calls fail, drmGetDeviceNameFromFd2() returns NULL, and
// aquamarine does `gpuName = drmName;` on the null char* -> strlen(NULL) -> the hard fault.
//
// The shim reports the synthetic node as major 226; minor is 0 for card0 and 128 for renderD128
// (both resolve to this same synthetic device), so we answer every minor under 226:.
// Return:
//   0 = not ours
//   1 = /device/drm         char device (the "is a DRM device" probe)
//   2 = /uevent             char-level uevent file (DEVNAME= for drmGetDeviceNameFromFd2)
//   3 = "" or /device       directory
//   4 = /device/subsystem   symlink (bus type for drmParseSubsystemType)
//   5 = /device/uevent      device-level uevent (PCI_SLOT_NAME= for drmParsePciBusInfo)
//   6 = /device/{vendor,device,subsystem_vendor,subsystem_device}  PCI id files
int drm_sysfs_kind(const char* path) {
  if (!path) return 0;
  static const char pre[] = "/sys/dev/char/226:";
  if (strncmp(path, pre, sizeof pre - 1)) return 0;
  const char* p = path + sizeof pre - 1;
  while (*p >= '0' && *p <= '9') p++;                 // skip the minor digits
  if (strcmp(p, "/device/drm") == 0) return 1;
  if (strcmp(p, "/uevent") == 0)     return 2;
  if (*p == 0 || strcmp(p, "/device") == 0) return 3;
  if (strcmp(p, "/device/subsystem") == 0) return 4;
  if (strcmp(p, "/device/uevent") == 0) return 5;
  if (strcmp(p, "/device/vendor") == 0 ||
      strcmp(p, "/device/device") == 0 ||
      strcmp(p, "/device/revision") == 0 ||
      strcmp(p, "/device/subsystem_vendor") == 0 ||
      strcmp(p, "/device/subsystem_device") == 0) return 6;
  return 0;
}

// Content served for /sys/dev/char/226:*/uevent. libdrm reads DEVNAME and prepends "/dev/",
// so the value must be the path relative to /dev ("dri/card0") for the returned name to be an
// openable devnode (aquamarine reopens it with open(name) in reopenDRMNode()).
const char* drm_sysfs_uevent(void) { return "DEVNAME=dri/card0\n"; }

// Target reported for /sys/dev/char/226:*/device/subsystem. libdrm's drmParseSubsystemType
// readlink()s this and maps the trailing path component to a bus type ("/pci" -> PCI), so the
// synthetic device enumerates as a PCI GPU. (Apple's AGX is PCIe, matching the "asahi" name.)
const char* drm_sysfs_link(void) { return "/pci"; }

// File body served for the device-level PCI files (kinds 5 and 6). A single PCI slot is all
// libdrm needs; both card0 and renderD128 report the same bus so drmFoldDuplicatedDevices merges
// them into one device carrying PRIMARY|RENDER nodes.
const char* drm_sysfs_pci_content(const char* path) {
  const char* p = strrchr(path, '/');
  if (!p) return 0;
  p++;                                              // leaf file name
  if (strcmp(p, "uevent") == 0)
     return "PCI_SLOT_NAME=0000:00:00.0\n";
  if (strcmp(p, "vendor") == 0)            return "0x106b\n";   // Apple
  if (strcmp(p, "device") == 0)            return "0x0001\n";
  if (strcmp(p, "revision") == 0)          return "0x00\n";
  if (strcmp(p, "subsystem_vendor") == 0)  return "0x106b\n";
  if (strcmp(p, "subsystem_device") == 0)  return "0x0001\n";
  return 0;
}

void drm_fake_stat(unsigned int* mode, unsigned long* rdev, const char* path) {
  int k = drm_sysfs_kind(path);
  if (k == 1) { *mode = 0020000u; *rdev = ((unsigned long)DRM_MAJOR << 8); return; } // S_IFCHR
  if (k == 2 || k == 5 || k == 6) { *mode = 0100000u; *rdev = 0; return; }           // S_IFREG
  if (k == 3) { *mode = 0040000u; *rdev = 0; return; }                               // S_IFDIR
  if (k == 4) { *mode = 0120000u; *rdev = 0; return; }                               // S_IFLNK
  int render = path && strstr(path, "renderD") != NULL;
  *mode = (*mode & 0777u) | 0020000u;                       // S_IFCHR
  *rdev = ((unsigned long)DRM_MAJOR << 8) | (render ? 128u : 0u);
}

// The single plausible output: a 1920x1080@60 preferred mode, written into a
// caller-provided struct drm_mode_modeinfo slot (MODE_GETCONNECTOR modes_ptr).
static void fill_modeinfo(struct drm_mode_modeinfo* m) {
  memset(m, 0, sizeof *m);
  m->clock       = 148500;                    // kHz
  m->hdisplay    = 1920; m->hsync_start = 2008; m->hsync_end = 2052; m->htotal = 2200; m->hskew = 0;
  m->vdisplay    = 1080; m->vsync_start = 1084; m->vsync_end = 1089; m->vtotal = 1125; m->vscan = 0;
  m->vrefresh    = 60;
  m->flags       = 0;
  m->type        = DRM_MODE_TYPE_PREFERRED;
  memcpy(m->name, "1920x1080", sizeof("1920x1080"));
}

long drm_ioctl(int fd, unsigned long req, unsigned long arg) {
  unsigned dir = IOC_DIR(req), type = IOC_TYPE(req), nr = IOC_NR(req), sz = IOC_SIZE(req);
  const char* kind = "unknown";
  if (type == DRM_TYPE) kind = (nr >= DRM_COMMAND_BASE) ? "ASAHI" : "drm-core";

  if (type == DRM_TYPE && nr == DRM_NR_VERSION && arg) {
    struct drm_version_64* v = (struct drm_version_64*)arg;
    v->major = 1; v->minor = 0; v->patchlevel = 0;
    put_str(v->name, &v->name_len, "rdmetal");
    put_str(v->date, &v->date_len, "20240101");
    put_str(v->desc, &v->desc_len, "rdmetal (shim Metal software bootstrap)");
    if (tracing()) fprintf(stderr, "[drm] VERSION -> rdmetal\n");
    return 0;
  }

  // ---- Asahi UAPI: minimal emulation (see the header block above) -------------------------

  // NOTE: nr 0xc6 is NOT an Asahi UAPI ioctl — it is DRM_IOCTL_MODE_CREATE_LEASE (KMS). The
  // shim's fallback returns -EINVAL for it, and aquamarine's reopenDRMNode() handles that with a
  // graceful "drmModeCreateLease failed, falling back to open" — so leaving it unimplemented is
  // CORRECT. The real Asahi UAPI is nr 0x40..0x4a (matching the rootfs header).

  // GET_PARAMS — the first wall. Return a plausible G13 (Apple M1) GPU so Mesa's device init
  // proceeds. The driver passes its own sizeof(drm_asahi_params_global) in p->size; we write at
  // most that many bytes into p->pointer, which is exactly how the kernel honours old userspace.
  if (type == DRM_TYPE && nr == ASAHI_NR_GET_PARAMS && arg) {
    struct asahi_get_params* p = (struct asahi_get_params*)arg;
    struct asahi_params_global g;
    memset(&g, 0, sizeof g);
    g.gpu_generation = 13;
    g.gpu_variant    = 'G';
    g.gpu_revision   = 0x00;
    g.chip_id        = 0x8103;                 // T8103 = Apple M1
    g.num_dies       = 1;
    g.num_clusters_total = 8;
    g.num_cores_per_cluster = 4;
    g.max_frequency_khz = 1278000;
    for (int i = 0; i < 8; i++) g.core_masks[i] = 0xF;
    g.vm_start = 0x0000001000000000ULL;        // 64 GiB  (>= shader_base = 2^36)
    g.vm_end   = 0x0000004000000000ULL;        // 256 GiB (kernel carve-out is vm_end-32GiB)
    g.vm_kernel_min_size = 0x0000000800000000ULL;  // 32 GiB (driver max()es with 32 GiB anyway)
    g.max_commands_per_submission = 256;
    g.max_attachments = 8;
    g.command_timestamp_frequency_hz = 24000000ULL;

    size_t want = (size_t)p->size;
    if (want > sizeof g) want = sizeof g;
    if (p->pointer) memcpy((void*)(uintptr_t)p->pointer, &g, want);
    if (tracing())
      fprintf(stderr, "[drm] ASAHI GET_PARAMS group=%u bufsize=%llu -> gen=%u var=%c chip=0x%x "
              "clusters=%u vm=[0x%llx,0x%llx)\n",
              p->param_group, (unsigned long long)p->size, g.gpu_generation, (char)g.gpu_variant,
              g.chip_id, g.num_clusters_total, (unsigned long long)g.vm_start,
              (unsigned long long)g.vm_end);
    return 0;
  }

  if (type == DRM_TYPE && nr == ASAHI_NR_GET_TIME && arg) {
    struct asahi_get_time* t = (struct asahi_get_time*)arg;
    t->gpu_timestamp = 0;
    if (tracing()) fprintf(stderr, "[drm] ASAHI GET_TIME -> 0\n");
    return 0;
  }

  if (type == DRM_TYPE && nr == ASAHI_NR_VM_CREATE && arg) {
    struct asahi_vm_create* v = (struct asahi_vm_create*)arg;
    static uint32_t next_vm = 1;
    v->vm_id = next_vm++;
    if (tracing())
      fprintf(stderr, "[drm] ASAHI VM_CREATE kernel=[0x%llx,0x%llx) -> vm_id=%u\n",
              (unsigned long long)v->kernel_start, (unsigned long long)v->kernel_end, v->vm_id);
    return 0;
  }

  if (type == DRM_TYPE && nr == ASAHI_NR_VM_DESTROY && arg) {
    struct asahi_vm_destroy* v = (struct asahi_vm_destroy*)arg;
    if (tracing()) fprintf(stderr, "[drm] ASAHI VM_DESTROY vm_id=%u\n", v->vm_id);
    return 0;
  }

  if (type == DRM_TYPE && nr == ASAHI_NR_VM_BIND && arg) {
    struct asahi_vm_bind* v = (struct asahi_vm_bind*)arg;
    if (tracing())
      fprintf(stderr, "[drm] ASAHI VM_BIND vm_id=%u binds=%u stride=%u userptr=0x%llx (no-op)\n",
              v->vm_id, v->num_binds, v->stride, (unsigned long long)v->userptr);
    return 0;
  }

  if (type == DRM_TYPE && nr == ASAHI_NR_GEM_CREATE && arg) {
    struct asahi_gem_create* c = (struct asahi_gem_create*)arg;
    c->handle = 0;
    struct bo_slot* slot = NULL;
    for (int i = 0; i < MAX_BO; i++) if (!g_bo[i].used) { slot = &g_bo[i]; break; }
    if (slot) {
      size_t rlen = ((size_t)c->size + 4095) & ~(size_t)4095;
      if (rlen == 0) rlen = 4096;
      int bfd = -1;
      void* m = bo_backing_map(rlen, &bfd);
      if (m != MAP_FAILED) {
        slot->handle = g_next_bo_handle;
        slot->map = m;
        slot->backing_fd = bfd;
        slot->size = (size_t)c->size;
        slot->used = 1;
        slot->refs = 1;
        c->handle = g_next_bo_handle++;
      }
    }
    if (!c->handle) {
      if (tracing()) fprintf(stderr, "[drm] ASAHI GEM_CREATE size=%llu -> -ENOMEM\n",
                             (unsigned long long)c->size);
      return -12;                                   // -ENOMEM
    }
    if (tracing())
      fprintf(stderr, "[drm] ASAHI GEM_CREATE size=%llu flags=0x%x vm_id=%u -> handle=0x%x\n",
              (unsigned long long)c->size, c->flags, c->vm_id, c->handle);
    return 0;
  }

  if (type == DRM_TYPE && nr == ASAHI_NR_GEM_MMAP_OFFSET && arg) {
    struct asahi_gem_mmap_offset* m = (struct asahi_gem_mmap_offset*)arg;
    struct bo_slot* b = bo_find(m->handle);
    m->offset = b ? ((uint64_t)m->handle << 12) : 0;
    if (tracing())
      fprintf(stderr, "[drm] ASAHI GEM_MMAP_OFFSET handle=0x%x -> offset=0x%llx%s\n",
              m->handle, (unsigned long long)m->offset, b ? "" : " (-EINVAL)");
    return b ? 0 : -22;
  }

  // MODE_CREATE_DUMB: allocate a linear CPU-mapped framebuffer for the sw winsys.
  // The backing is an anonymous file mapped MAP_SHARED (bo_backing_map); the handle
  // lives in the same g_bo[] namespace as ASAHI GEM_CREATE so PRIME_HANDLE_TO_FD /
  // drm_bo_map_by_fd and the mmap (offset = handle << 12) all resolve to it unchanged.
  if (type == DRM_TYPE && nr == DRM_NR_MODE_CREATE_DUMB && arg) {
    struct drm_mode_create_dumb* c = (struct drm_mode_create_dumb*)arg;
    c->handle = 0;
    c->pitch = 0;
    c->size = 0;
    if (!c->width || !c->height || c->bpp < 8 || c->bpp % 8) {
      if (tracing()) fprintf(stderr, "[drm] MODE_CREATE_DUMB %ux%u bpp=%u -> -EINVAL\n",
                             c->width, c->height, c->bpp);
      return -22;
    }
    uint32_t pitch = (uint32_t)(((uint64_t)c->width * (c->bpp / 8) + 63u) & ~63u);
    uint64_t size = (uint64_t)pitch * c->height;
    struct bo_slot* slot = NULL;
    for (int i = 0; i < MAX_BO; i++) if (!g_bo[i].used) { slot = &g_bo[i]; break; }
    if (!slot || size == 0 || size > (1ull << 30)) {
      if (tracing()) fprintf(stderr, "[drm] MODE_CREATE_DUMB %ux%u -> -ENOMEM\n",
                             c->width, c->height);
      return -12;
    }
    size_t rlen = (size_t)((size + 4095) & ~(size_t)4095);
    int bfd = -1;
    void* m = bo_backing_map(rlen, &bfd);
    if (m == MAP_FAILED) {
      if (tracing()) fprintf(stderr, "[drm] MODE_CREATE_DUMB %ux%u -> -ENOMEM (mmap)\n",
                             c->width, c->height);
      return -12;
    }
    slot->handle = g_next_bo_handle;
    slot->map = m;
    slot->backing_fd = bfd;
    slot->size = (size_t)size;
    slot->used = 1;
    slot->refs = 1;
    c->handle = g_next_bo_handle++;
    c->pitch = pitch;
    c->size = size;
    if (tracing())
      fprintf(stderr, "[drm] MODE_CREATE_DUMB %ux%u bpp=%u -> handle=0x%x pitch=%u size=%llu\n",
              c->width, c->height, c->bpp, c->handle, c->pitch,
              (unsigned long long)c->size);
    return 0;
  }

  // MODE_MAP_DUMB: return the fake mmap offset for the handle. Reuses the GEM
  // scheme (offset = handle << 12) so syscall 222 (mmap) resolves through
  // drm_mmap_bo() to the same host backing.
  if (type == DRM_TYPE && nr == DRM_NR_MODE_MAP_DUMB && arg) {
    struct drm_mode_map_dumb* m = (struct drm_mode_map_dumb*)arg;
    struct bo_slot* b = bo_find(m->handle);
    m->offset = b ? ((uint64_t)m->handle << 12) : 0;
    if (tracing())
      fprintf(stderr, "[drm] MODE_MAP_DUMB handle=0x%x -> offset=0x%llx%s\n",
              m->handle, (unsigned long long)m->offset, b ? "" : " (-EINVAL)");
    return b ? 0 : -22;
  }

  // MODE_DESTROY_DUMB: drop the creator's reference on a dumb buffer.
  if (type == DRM_TYPE && nr == DRM_NR_MODE_DESTROY_DUMB && arg) {
    struct drm_mode_destroy_dumb* d = (struct drm_mode_destroy_dumb*)arg;
    struct bo_slot* b = bo_find(d->handle);
    if (tracing())
      fprintf(stderr, "[drm] MODE_DESTROY_DUMB handle=0x%x%s\n", d->handle, b ? "" : " (-EINVAL)");
    if (b) {
      if (b->refs > 1) { b->refs--; return 0; }
      export_forget(d->handle);
      bo_unref(b);
    }
    return 0;
  }

  if (type == DRM_TYPE && nr == ASAHI_NR_GEM_BIND_OBJECT && arg) {
    struct asahi_gem_bind_object* b = (struct asahi_gem_bind_object*)arg;
    if (b->op == 0) b->object_handle = 1;           // BIND -> mint an object handle
    if (tracing())
      fprintf(stderr, "[drm] ASAHI GEM_BIND_OBJECT op=%u flags=0x%x handle=0x%x -> obj=%u\n",
              b->op, b->flags, b->handle, b->object_handle);
    return 0;
  }

  if (type == DRM_TYPE && nr == ASAHI_NR_QUEUE_CREATE && arg) {
    struct asahi_queue_create* q = (struct asahi_queue_create*)arg;
    static uint32_t next_q = 1;
    q->queue_id = next_q++;
    if (tracing())
      fprintf(stderr, "[drm] ASAHI QUEUE_CREATE vm_id=%u prio=%u usc_base=0x%llx -> queue_id=%u\n",
              q->vm_id, q->priority, (unsigned long long)q->usc_exec_base, q->queue_id);
    return 0;
  }

  if (type == DRM_TYPE && nr == ASAHI_NR_QUEUE_DESTROY && arg) {
    struct asahi_queue_destroy* q = (struct asahi_queue_destroy*)arg;
    if (tracing()) fprintf(stderr, "[drm] ASAHI QUEUE_DESTROY queue_id=%u\n", q->queue_id);
    return 0;
  }

  // SUBMIT — no execution; accept and report success. The real GPU work is never performed here:
  // the shim's Metal present path owns pixels. Out-syncs are not signalled yet (see syncobj note),
  // which matters only once the compositor actually drives a render loop through this driver.
  if (type == DRM_TYPE && nr == ASAHI_NR_SUBMIT && arg) {
    struct asahi_submit* s = (struct asahi_submit*)arg;
    if (tracing())
      fprintf(stderr, "[drm] ASAHI SUBMIT queue=%u in=%u out=%u cmdbuf=0x%llx size=%u (no-op)\n",
              s->queue_id, s->in_sync_count, s->out_sync_count,
              (unsigned long long)s->cmdbuf, s->cmdbuf_size);
    return 0;
  }

  // PRIME_HANDLE_TO_FD: gbm_bo_get_fd_for_plane() exports a created BO as a dma-buf fd. The BO's
  // host backing is already the rendered frame (the guest runs in-process), so the fd returned
  // here is a virtual dma-buf carrier: an unlinked temp file sized to the BO, carrying identity
  // only (fstat(st_dev, st_ino)), like rdgpu_virtual_carrier.c. The identity is recorded against
  // the BO handle; the compositor reads the actual pixels through drm_bo_map_by_fd().
  if (type == DRM_TYPE && nr == DRM_NR_PRIME_HANDLE_TO_FD && arg) {
    struct drm_prime_handle* ph = (struct drm_prime_handle*)arg;
    ph->fd = -1;
    struct bo_slot* b = bo_find(ph->handle);
    if (!b) {
      if (tracing())
        fprintf(stderr, "[drm] PRIME_HANDLE_TO_FD handle=0x%x -> -EINVAL (unknown)\n", ph->handle);
      return -22;
    }
    char tmpl[] = "/tmp/rdmetal-dmabuf-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return -12;
    unlink(tmpl);
    size_t rlen = ((size_t)b->size + 4095) & ~(size_t)4095;
    if (rlen == 0) rlen = 4096;
    if (ftruncate(fd, (off_t)rlen)) { close(fd); return -12; }
    struct stat st;
    if (fstat(fd, &st) || st.st_dev == 0 || st.st_ino == 0) { close(fd); return -12; }
    // Honor DRM_CLOEXEC (the caller requested it): the SCM_RIGHTS receiver re-marks it anyway.
    int cfd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    close(fd);
    if (cfd < 0) return -12;
    export_remember(b->handle, st.st_dev, st.st_ino);
    // The exported dmabuf fd holds a reference to the BO: standard dmabuf semantics — the fd
    // keeps the buffer alive even after the creator's MODE_DESTROY_DUMB. Without this, KWin's
    // GBM allocator (create -> gbm_bo_get_fd -> gbm_bo_destroy -> eglCreateImageKHR) exports the
    // cursor/output buffer and immediately destroys the dumb BO, so the later import resolves to
    // a freed handle and MODE_MAP_DUMB fails (-EINVAL) -> null deref inside rdmetal_dri.so.
    bo_ref(b);
    ph->fd = cfd;
    fprintf(stderr, "DRM_EXPORT PRIME_HANDLE_TO_FD handle=0x%x size=%zu -> fd=%d dev=%d ino=%llu\n",
            b->handle, b->size, cfd, (int)st.st_dev, (unsigned long long)st.st_ino);
    return 0;
  }

  // PRIME_FD_TO_HANDLE: the compositor's dma-buf import path (wlsrv.c) resolves a transferred
  // guest dmabuf fd into an opaque GEM handle. Two cases: (1) the fd is one of OUR
  // PRIME_HANDLE_TO_FD carriers (in-process round-trip, the common Hyprland GBM path) — then the
  // handle is minted directly and no handoff is needed; (2) the fd is a cross-process carrier
  // from shim_cli, resolved to the real IOSurfaceRef through the mach-port handoff and stored in
  // a gem slot. If no surface is available the handle is still minted as a synthetic token.
  if (type == DRM_TYPE && nr == DRM_NR_PRIME_FD_TO_HANDLE && arg) {
    struct drm_prime_handle* ph = (struct drm_prime_handle*)arg;
    static uint32_t g_next_handle = 1;

    struct stat st;
    int have_id = !fstat(ph->fd, &st) && st.st_dev && st.st_ino;
    if (have_id && export_find(st.st_dev, st.st_ino)) {
      // Own export: the backing BO is already in this process. Return the ORIGINAL
      // handle (not a fresh minted one) so the sw winsys's import (find_and_ref by
      // handle) reuses the SAME BO and softpipe renders into the SAME host backing.
      struct export_slot* e = export_find(st.st_dev, st.st_ino);
      struct bo_slot* b = bo_find(e->handle);
      ph->handle = e->handle;
      if (b) bo_ref(b);                     // importer holds a reference
      fprintf(stderr, "DRM_IMPORT_PRIME fd=%d -> handle=0x%x (own export)\n", ph->fd, ph->handle);
      return 0;
    }
    if (have_id) {
      // A repeated import of the same carrier fd returns the same handle (dedup by
      // st_dev/st_ino, the stable identity across dup/SCM_RIGHTS).
      struct gem_slot* existing = gem_find_by_identity(st.st_dev, st.st_ino);
      if (existing) {
        ph->handle = existing->handle;
        fprintf(stderr, "DRM_IMPORT_IOSURFACE fd=%d handle=%u resolved=%d id=%u\n",
                ph->fd, ph->handle, existing->resolved,
                existing->surface ? IOSurfaceGetID(existing->surface) : 0u);
        return 0;
      }
    }

    // Resolve the backing IOSurface through the mach-port handoff. The export side
    // (shim_cli mock_fd_export) published the send-right via IOSurfaceCreateMachPort keyed by
    // the carrier's (st_dev, st_ino); rdgpu_handoff_resolve_fd() redeems it with
    // IOSurfaceLookupFromMachPort() and returns a +1 reference the gem slot owns. In-process
    // publish->resolve also lands here via the handoff's process-local table.
    IOSurfaceRef surface = rdgpu_handoff_resolve_fd(ph->fd);

    ph->handle = g_next_handle++;
    struct gem_slot* slot = NULL;
    for (int i = 0; i < MAX_GEM; i++)
      if (!g_gem[i].resolved) { slot = &g_gem[i]; break; }
    if (slot) {
      slot->handle = ph->handle;
      slot->fd = ph->fd;
      slot->device = have_id ? st.st_dev : 0;
      slot->inode = have_id ? st.st_ino : 0;
      slot->surface = surface;   // NULL on failure; +1 reference otherwise
      slot->resolved = 1;
    } else if (surface) {
      CFRelease(surface);         // no slot to store it
      surface = NULL;
    }

    fprintf(stderr, "DRM_IMPORT shim PRIME_FD_TO_HANDLE fd=%d -> handle=%u\n", ph->fd, ph->handle);
    fprintf(stderr, "DRM_IMPORT_IOSURFACE fd=%d handle=%u resolved=%d id=%u\n",
            ph->fd, ph->handle, surface != NULL,
            surface ? IOSurfaceGetID(surface) : 0u);
    if (tracing())
      fprintf(stderr, "[drm] PRIME_FD_TO_HANDLE fd=%d -> handle=%u\n", ph->fd, ph->handle);
    return 0;
  }

  // GEM_CLOSE: release an imported surface's reference or a created BO's backing.
  if (type == DRM_TYPE && nr == DRM_NR_GEM_CLOSE && arg) {
    struct drm_gem_close* gc = (struct drm_gem_close*)arg;
    struct gem_slot* slot = gem_find_by_handle(gc->handle);
    if (slot) {
      if (tracing())
        fprintf(stderr, "[drm] GEM_CLOSE handle=%u (release surface)\n", gc->handle);
      gem_release_slot(slot);
      return 0;
    }
    struct bo_slot* b = bo_find(gc->handle);
    if (b) {
      if (tracing())
        fprintf(stderr, "[drm] GEM_CLOSE handle=0x%x (release BO %zu bytes, refs=%u)\n",
                gc->handle, b->size, b->refs);
      if (b->refs > 1) {
        b->refs--;
        return 0;
      }
      export_forget(gc->handle);
      bo_unref(b);
      return 0;
    }
    if (tracing()) fprintf(stderr, "[drm] GEM_CLOSE handle=%u (unknown, no-op)\n", gc->handle);
    return 0;
  }

  // ---- KMS surface (aquamarine checkFeatures + resource scan) -----------------

  // GET_CAP: answer the capabilities aquamarine probes. PRIME must carry
  // DRM_PRIME_CAP_IMPORT or checkFeatures() bails and m_drmFD stays < 0.
  if (type == DRM_TYPE && nr == DRM_NR_GET_CAP && arg) {
    struct drm_get_cap* gc = (struct drm_get_cap*)arg;
    uint64_t v = 0;
    int ok = 1;
    switch (gc->capability) {
      case DRM_CAP_DUMB_BUFFER:         v = 1; break;
      case DRM_CAP_PRIME:               v = DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT; break;
      case DRM_CAP_TIMESTAMP_MONOTONIC: v = 1; break;
      case DRM_CAP_ASYNC_PAGE_FLIP:     v = 1; break;
      case DRM_CAP_CRTC_IN_VBLANK_EVENT: v = 1; break;
      case DRM_CAP_CURSOR_WIDTH:        v = 64; break;
      case DRM_CAP_CURSOR_HEIGHT:       v = 64; break;
      case DRM_CAP_ADDFB2_MODIFIERS:    v = 1; break;
      case DRM_CAP_SYNCOBJ:             v = 1; break;         // 0x13
      case DRM_CAP_SYNCOBJ_TIMELINE:    v = 1; break;         // 0x14
      default:                          ok = 0; break;
    }
    gc->value = v;
    if (tracing())
      fprintf(stderr, "[drm] GET_CAP cap=0x%llx -> %s value=%llu\n",
              (unsigned long long)gc->capability, ok ? "ok" : "-EINVAL",
              (unsigned long long)v);
    return ok ? 0 : -22;
  }

  // SET_CLIENT_CAP: UNIVERSAL_PLANES and ATOMIC both succeed (atomic path on).
  if (type == DRM_TYPE && nr == DRM_NR_SET_CLIENT_CAP && arg) {
    struct drm_set_client_cap* sc = (struct drm_set_client_cap*)arg;
    int ok = (sc->capability == DRM_CLIENT_CAP_UNIVERSAL_PLANES ||
              sc->capability == DRM_CLIENT_CAP_ATOMIC);
    if (tracing())
      fprintf(stderr, "[drm] SET_CLIENT_CAP cap=%llu value=%llu -> %s\n",
              (unsigned long long)sc->capability, (unsigned long long)sc->value,
              ok ? "ok" : "-EINVAL");
    return ok ? 0 : -22;
  }

  // MODE_GETRESOURCES: one fb/CRTC/connector/encoder. The kernel two-call
  // protocol: first call has NULL id pointers (counts only), the second has
  // arrays we fill. Answering both with the same counts keeps it idempotent.
  if (type == DRM_TYPE && nr == DRM_NR_MODE_GETRESOURCES && arg) {
    struct drm_mode_card_res* r = (struct drm_mode_card_res*)arg;
    r->count_fbs = 1;
    r->count_crtcs = 1;
    r->count_connectors = 1;
    r->count_encoders = 1;
    r->min_width = 1; r->max_width = 16384;
    r->min_height = 1; r->max_height = 16384;
    if (r->fb_id_ptr)        *(uint32_t*)(uintptr_t)r->fb_id_ptr = 1;
    if (r->crtc_id_ptr)      *(uint32_t*)(uintptr_t)r->crtc_id_ptr = 1;
    if (r->connector_id_ptr) *(uint32_t*)(uintptr_t)r->connector_id_ptr = 1;
    if (r->encoder_id_ptr)   *(uint32_t*)(uintptr_t)r->encoder_id_ptr = 1;
    if (tracing())
      fprintf(stderr, "[drm] MODE_GETRESOURCES fbs=%u crtcs=%u connectors=%u encoders=%u\n",
              r->count_fbs, r->count_crtcs, r->count_connectors, r->count_encoders);
    return 0;
  }

  // MODE_GETCRTC: a CRTC with no active mode yet (mode_valid=0).
  if (type == DRM_TYPE && nr == DRM_NR_MODE_GETCRTC && arg) {
    struct drm_mode_get_crtc* c = (struct drm_mode_get_crtc*)arg;
    c->set_connectors_ptr = 0;
    c->count_connectors = 0;
    c->fb_id = 0;
    c->x = 0; c->y = 0;
    c->gamma_size = 0;
    c->mode_valid = 0;
    memset(&c->mode, 0, sizeof c->mode);
    if (tracing())
      fprintf(stderr, "[drm] MODE_GETCRTC crtc_id=%u (no active mode)\n", c->crtc_id);
    return 0;
  }

  // MODE_GETENCODER: one virtual encoder wired to crtc 1.
  if (type == DRM_TYPE && nr == DRM_NR_MODE_GETENCODER && arg) {
    struct drm_mode_get_encoder* e = (struct drm_mode_get_encoder*)arg;
    e->encoder_type = DRM_MODE_ENCODER_VIRTUAL;
    e->crtc_id = 1;
    e->possible_crtcs = 1;   // bit 0 -> crtc 1
    e->possible_clones = 0;
    if (tracing())
      fprintf(stderr, "[drm] MODE_GETENCODER encoder_id=%u type=VIRTUAL crtc_id=%u\n",
              e->encoder_id, e->crtc_id);
    return 0;
  }

  // MODE_GETCONNECTOR: one connected eDP connector with one 1920x1080 mode.
  if (type == DRM_TYPE && nr == DRM_NR_MODE_GETCONNECTOR && arg) {
    struct drm_mode_get_connector* c = (struct drm_mode_get_connector*)arg;
    c->encoder_id = 1;
    c->connector_type = DRM_MODE_CONNECTOR_eDP;
    c->connector_type_id = 1;
    c->connection = DRM_MODE_CONNECTED;
    c->mm_width = 340; c->mm_height = 190;
    c->subpixel = 0;
    c->pad = 0;
    c->count_modes = 1;
    c->count_props = 0;
    c->count_encoders = 1;
    if (c->encoders_ptr) *(uint32_t*)(uintptr_t)c->encoders_ptr = 1;
    if (c->modes_ptr)    fill_modeinfo((struct drm_mode_modeinfo*)(uintptr_t)c->modes_ptr);
    // count_props == 0, so props_ptr/prop_values_ptr stay untouched.
    if (tracing())
      fprintf(stderr, "[drm] MODE_GETCONNECTOR connector_id=%u type=eDP connection=%u modes=%u encoders=%u\n",
              c->connector_id, c->connection, c->count_modes, c->count_encoders);
    return 0;
  }

  // MODE_GETPLANERESOURCES: one primary plane.
  if (type == DRM_TYPE && nr == DRM_NR_MODE_GETPLANERESOURCES && arg) {
    struct drm_mode_get_plane_res* p = (struct drm_mode_get_plane_res*)arg;
    p->count_planes = 1;
    if (p->plane_id_ptr) *(uint32_t*)(uintptr_t)p->plane_id_ptr = 1;
    if (tracing())
      fprintf(stderr, "[drm] MODE_GETPLANERESOURCES planes=%u\n", p->count_planes);
    return 0;
  }

  // MODE_ADDFB2 / MODE_RMFB: mint and accept fb ids (the reference fixture has
  // these; ADDFB2 is exercised by aquamarine's buffer path after checkFeatures).
  if (type == DRM_TYPE && nr == DRM_NR_MODE_ADDFB2 && arg) {
    struct drm_mode_fb_cmd2* fb = (struct drm_mode_fb_cmd2*)arg;
    fb->fb_id = g_next_fb++;
    if (g_next_fb == 0) g_next_fb = 1;
    if (tracing())
      fprintf(stderr, "[drm] MODE_ADDFB2 -> fb_id=%u\n", fb->fb_id);
    return 0;
  }

  if (type == DRM_TYPE && nr == DRM_NR_MODE_RMFB && arg) {
    if (tracing())
      fprintf(stderr, "[drm] MODE_RMFB fb_id=%u\n", *(uint32_t*)arg);
    return 0;
  }

  // MODE_GETPLANE: one primary plane (id 1) compatible with crtc 1, exposing
  // the XRGB8888 format the ADDFB2 path accepts.
  if (type == DRM_TYPE && nr == DRM_NR_MODE_GETPLANE && arg) {
    struct drm_mode_get_plane* pl = (struct drm_mode_get_plane*)arg;
    pl->crtc_id = 1;
    pl->fb_id = 0;
    pl->possible_crtcs = 1;          // bit 0 -> crtc 1
    pl->gamma_size = 0;
    pl->count_format_types = 1;
    if (pl->format_type_ptr)
      *(uint32_t*)(uintptr_t)pl->format_type_ptr = DRM_FORMAT_XRGB8888;
    if (tracing())
      fprintf(stderr, "[drm] MODE_GETPLANE plane_id=%u crtc_id=%u formats=%u\n",
              pl->plane_id, pl->crtc_id, pl->count_format_types);
    return 0;
  }

  // MODE_OBJ_GETPROPERTIES: only the plane carries a property — "type" (id 1)
  // valued DRM_PLANE_TYPE_PRIMARY. This is what lets SDRMPlane::init() resolve
  // the plane type via getDRMProp(); CRTC/connector report zero properties,
  // which scanProperties() accepts (non-NULL empty object).
  if (type == DRM_TYPE && nr == DRM_NR_MODE_OBJ_GETPROPERTIES && arg) {
    struct drm_mode_obj_get_properties* o = (struct drm_mode_obj_get_properties*)arg;
    int is_plane = (o->obj_type == DRM_MODE_OBJECT_PLANE) ||
                   (o->obj_type == DRM_MODE_OBJECT_ANY);
    o->count_props = is_plane ? 1 : 0;
    if (is_plane) {
      if (o->props_ptr)       *(uint32_t*)(uintptr_t)o->props_ptr = 1;
      if (o->prop_values_ptr) *(uint64_t*)(uintptr_t)o->prop_values_ptr = DRM_PLANE_TYPE_PRIMARY;
    }
    if (tracing())
      fprintf(stderr, "[drm] MODE_OBJ_GETPROPERTIES obj_id=%u obj_type=0x%x count=%u\n",
              o->obj_id, o->obj_type, o->count_props);
    return 0;
  }

  // MODE_GETPROPERTY: property 1 is the plane "type" enum; nothing else exists.
  if (type == DRM_TYPE && nr == DRM_NR_MODE_GETPROPERTY && arg) {
    struct drm_mode_get_property* p = (struct drm_mode_get_property*)arg;
    if (p->prop_id == 1) {
      p->flags = DRM_MODE_PROP_ENUM | DRM_MODE_PROP_IMMUTABLE | DRM_MODE_PROP_ATOMIC;
      memcpy(p->name, "type", sizeof("type"));
      p->count_values = 0;
      p->count_enum_blobs = 0;
      if (tracing())
        fprintf(stderr, "[drm] MODE_GETPROPERTY prop_id=1 -> \"type\"\n");
      return 0;
    }
    if (tracing())
      fprintf(stderr, "[drm] MODE_GETPROPERTY prop_id=%u -> -EINVAL\n", p->prop_id);
    return -22;
  }

  // MODE_GETPROPBLOB: no property blobs are exposed (IN_FORMATS/EDID/mode_id all
  // absent), so any query is answered with an empty blob.
  if (type == DRM_TYPE && nr == DRM_NR_MODE_GETPROPBLOB && arg) {
    struct drm_mode_get_blob* b = (struct drm_mode_get_blob*)arg;
    b->length = 0;
    if (tracing())
      fprintf(stderr, "[drm] MODE_GETPROPBLOB blob_id=%u -> empty\n", b->blob_id);
    return 0;
  }

  // ---- drm_syncobj (core DRM, nr 0xbf..0xcd): the asahi driver's explicit-sync primitives ----
  // The shim never executes GPU work, so a syncobj is always "signalled": WAIT returns
  // immediately, SIGNAL/RESET are no-ops. Only the handle bookkeeping must be coherent.
  {
    static uint32_t g_next_syncobj = 0x1000;
    if (type == DRM_TYPE && nr == 0xbf && arg) {                       // SYNCOBJ_CREATE
      struct { uint32_t handle; uint32_t flags; }* s = (void*)(uintptr_t)arg;
      s->handle = g_next_syncobj++;
      if (tracing()) fprintf(stderr, "[drm] SYNCOBJ_CREATE -> handle=%u\n", s->handle);
      return 0;
    }
    if (type == DRM_TYPE && nr == 0xc0 && arg) {                       // SYNCOBJ_DESTROY
      if (tracing()) fprintf(stderr, "[drm] SYNCOBJ_DESTROY handle=%u\n", *(uint32_t*)arg);
      return 0;
    }
    if (type == DRM_TYPE && nr == 0xc2 && arg) {                       // SYNCOBJ_FD_TO_HANDLE
      struct { uint32_t handle; uint32_t flags; int32_t fd; uint32_t pad; }* s = (void*)(uintptr_t)arg;
      s->handle = g_next_syncobj++;
      if (tracing()) fprintf(stderr, "[drm] SYNCOBJ_FD_TO_HANDLE fd=%d -> handle=%u\n", s->fd, s->handle);
      return 0;
    }
    if (type == DRM_TYPE && nr == 0xc1 && arg) {                       // SYNCOBJ_HANDLE_TO_FD
      struct { uint32_t handle; uint32_t flags; int32_t fd; uint32_t pad; }* s = (void*)(uintptr_t)arg;
      s->fd = open("/dev/null", O_RDWR);
      if (tracing()) fprintf(stderr, "[drm] SYNCOBJ_HANDLE_TO_FD handle=%u -> fd=%d\n", s->handle, s->fd);
      return s->fd >= 0 ? 0 : -12;
    }
    if (type == DRM_TYPE && nr == 0xc3 && arg) {                       // SYNCOBJ_WAIT
      struct { uint64_t handles; int64_t timeout_nsec; uint32_t count_handles;
               uint32_t flags; uint32_t first_signaled; uint32_t pad; }* w = (void*)(uintptr_t)arg;
      w->first_signaled = 0;                                           // all signalled
      if (tracing()) fprintf(stderr, "[drm] SYNCOBJ_WAIT count=%u -> signalled\n", w->count_handles);
      return 0;
    }
    if (type == DRM_TYPE && nr == 0xca && arg) {                       // SYNCOBJ_TIMELINE_WAIT
      struct { uint64_t handles; uint64_t points; int64_t timeout_nsec; uint32_t count_handles;
               uint32_t flags; uint32_t first_signaled; uint32_t pad; }* w = (void*)(uintptr_t)arg;
      w->first_signaled = 0;
      if (tracing()) fprintf(stderr, "[drm] SYNCOBJ_TIMELINE_WAIT count=%u -> signalled\n", w->count_handles);
      return 0;
    }
    if ((type == DRM_TYPE && nr == 0xc4) ||                            // SYNCOBJ_RESET
        (type == DRM_TYPE && nr == 0xc5) ||                            // SYNCOBJ_SIGNAL
        (type == DRM_TYPE && nr == 0xcd) ||                            // SYNCOBJ_TIMELINE_SIGNAL
        (type == DRM_TYPE && nr == 0xcb) ||                            // SYNCOBJ_QUERY
        (type == DRM_TYPE && nr == 0xcc)) {                            // SYNCOBJ_TRANSFER
      if (tracing()) fprintf(stderr, "[drm] SYNCOBJ op nr=0x%02x (no-op)\n", nr);
      return 0;
    }
  }

  if (tracing()) {
    fprintf(stderr, "[drm] fd=%d %-8s dir=%u type=%c nr=0x%02x%s size=%-4u  -> -EINVAL "
                    "(not implemented yet)\n",
            fd, kind, dir, type ? (char)type : '?', nr,
            (type == DRM_TYPE && nr >= DRM_COMMAND_BASE) ? "" : "     ", sz);
    if (type == DRM_TYPE && arg && sz >= 4 && sz <= 512)
      trace_hex("  (arg payload)", (void*)(uintptr_t)arg, sz);
  }
  return -22;                                    // -EINVAL
}
