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

#include "rdgpu_native_output.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurface.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

// Present bridge to the Metal window. Declared + defined WEAK here so the headless builds
// (shim_cli, shim_wlh) link without window.m; the GUI build's strong definition in window.m
// overrides this no-op with the real CAMetalLayer present path (the exact call the Wayland
// path reaches via x11rnd_present_bgra -> gpuwin_present_fmt(..., bgra=1)).
__attribute__((weak)) long gpuwin_present_fmt(const uint8_t* px, uint64_t w, uint64_t h, int bgra) {
  (void)px; (void)w; (void)h; (void)bgra;
  return -19;                                   // no Metal present path in this build
}

// Per-CRTC present bridge. Declared + defined WEAK so headless builds (shim_cli, shim_wlh)
// link without window.m; the GUI build's strong definition in window.m overrides this with a
// route that sends crtc 1 to the primary Metal window and crtc 2 to the secondary output.
// `crtc_id` is the shim's CRTC object id (1 = external DP, 2 = builtin eDP).
__attribute__((weak)) long gpuwin_present_crtc(const uint8_t* px, uint64_t w, uint64_t h, int bgra,
                                               uint32_t crtc_id) {
  (void)crtc_id;
  return gpuwin_present_fmt(px, w, h, bgra);   // headless fallback: primary only
}

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
#define DRM_NR_MODE_SETCRTC          0xa2
#define DRM_NR_MODE_SETPROPERTY      0xab   // legacy drmModeConnectorSetProperty (DPMS)
#define DRM_NR_MODE_PAGE_FLIP        0xb0
#define DRM_NR_MODE_ATOMIC           0xbc
#define DRM_NR_MODE_CREATEPROPBLOB   0xbd
#define DRM_NR_MODE_DESTROYPROPBLOB  0xbe
#define DRM_NR_MODE_OBJ_SETPROPERTY  0xba

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
#define DRM_MODE_CONNECTOR_DisplayPort 10
#define DRM_MODE_CONNECTOR_eDP    14
#define DRM_MODE_ENCODER_VIRTUAL  5
#define DRM_MODE_CONNECTED        1
#define DRM_MODE_DISCONNECTED     2
#define DRM_MODE_TYPE_PREFERRED   8

// Property surface (drm_mode.h). One primary plane, one CRTC, one connector, each
// exposing the standard atomic properties aquamarine discovers by NAME via
// MODE_OBJ_GETPROPERTIES + MODE_GETPROPERTY and then references in MODE_ATOMIC.
#define DRM_MODE_OBJECT_CRTC      0xcccccccc
#define DRM_MODE_OBJECT_CONNECTOR 0xc0c0c0c0
#define DRM_MODE_OBJECT_PLANE     0xeeeeeeee
#define DRM_MODE_OBJECT_ANY       0
#define DRM_MODE_PROP_PENDING     (1 << 0)
#define DRM_MODE_PROP_RANGE       (1 << 1)
#define DRM_MODE_PROP_IMMUTABLE   (1 << 2)
#define DRM_MODE_PROP_ENUM        (1 << 3)
#define DRM_MODE_PROP_BLOB        (1 << 4)
#define DRM_MODE_PROP_OBJECT      (DRM_MODE_PROP_TYPE(1))
#define DRM_MODE_PROP_TYPE(n)     ((n) << 6)
#define DRM_MODE_PROP_ATOMIC      0x80000000
#define DRM_PLANE_TYPE_PRIMARY    1
#define DRM_PLANE_TYPE_CURSOR     2
#define DRM_PLANE_TYPE_OVERLAY    3
#define DRM_FORMAT_XRGB8888       0x34325258   // fourcc 'XR24'

// Atomic commit flags (drm_mode.h) and page-flip flags.
#define DRM_MODE_PAGE_FLIP_EVENT      0x01
#define DRM_MODE_PAGE_FLIP_ASYNC      0x02
#define DRM_MODE_ATOMIC_TEST_ONLY     0x0100
#define DRM_MODE_ATOMIC_NONBLOCK      0x0200
#define DRM_MODE_ATOMIC_ALLOW_MODESET 0x0400

// DRM events (drm.h).
#define DRM_EVENT_VBLANK        0x01
#define DRM_EVENT_FLIP_COMPLETE 0x02

// Property ids (shim-internal global namespace; stable across MODE_OBJ_GETPROPERTIES,
// MODE_GETPROPERTY and MODE_ATOMIC).
#define DRM_PROP_TYPE     1    // plane "type" enum (immutable)
#define DRM_PROP_ACTIVE   2    // crtc "ACTIVE" range
#define DRM_PROP_MODE_ID  3    // crtc "MODE_ID" blob
#define DRM_PROP_CRTC_ID  4    // connector+plane "CRTC_ID" object
#define DRM_PROP_FB_ID    5    // plane "FB_ID" object
#define DRM_PROP_SRC_X    6    // plane range (16.16)
#define DRM_PROP_SRC_Y    7
#define DRM_PROP_SRC_W    8
#define DRM_PROP_SRC_H    9
#define DRM_PROP_CRTC_X   10   // plane range (px)
#define DRM_PROP_CRTC_Y   11
#define DRM_PROP_CRTC_W   12
#define DRM_PROP_CRTC_H   13
#define DRM_PROP_DPMS     14   // connector "DPMS" enum

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

// ---- KMS commit structs (drm_mode.h, 64-bit aarch64 layouts). ----
// drm_mode_crtc doubles as both MODE_GETCRTC (output) and MODE_SETCRTC (input).
struct drm_mode_crtc_page_flip {
  uint32_t crtc_id, fb_id, flags, reserved;                                          // 16
  uint64_t user_data;                                                                // 8 -> 24
};
struct drm_mode_atomic {
  uint32_t flags, count_objs;                                                        // 8
  uint64_t objs_ptr, count_props_ptr, props_ptr, prop_values_ptr, reserved, user_data; // 48 -> 56
};
struct drm_mode_create_blob {
  uint64_t data;                                                                     // 8
  uint32_t length, blob_id;                                                          // 8 -> 16
};
struct drm_mode_destroy_blob {
  uint32_t blob_id;                                                                  // 4
};
struct drm_mode_property_enum {
  uint64_t value;                                                                    // 8
  char name[32];                                                                     // 32 -> 40
};
struct drm_mode_connector_set_property {
  uint64_t value;                                                                    // 8
  uint32_t prop_id, connector_id;                                                    // 8 -> 16
};
struct drm_mode_obj_set_property {
  uint64_t value;                                                                    // 8
  uint32_t prop_id, obj_id, obj_type;                                                // 12 -> 20
};
// DRM events delivered via read() on the DRM fd after a page-flip / atomic commit.
struct drm_event {
  uint32_t type, length;
};
struct drm_event_vblank {
  struct drm_event base;                                                             // 8
  uint64_t user_data;                                                                // 8 -> 16
  uint32_t tv_sec, tv_usec, sequence, crtc_id;                                       // 16 -> 32
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
_Static_assert(sizeof(struct drm_mode_crtc_page_flip) == 24, "drm_mode_crtc_page_flip");
_Static_assert(sizeof(struct drm_mode_atomic)       == 56, "drm_mode_atomic");
_Static_assert(sizeof(struct drm_mode_create_blob)  == 16, "drm_mode_create_blob");
_Static_assert(sizeof(struct drm_mode_destroy_blob) == 4,  "drm_mode_destroy_blob");
_Static_assert(sizeof(struct drm_mode_property_enum) == 40, "drm_mode_property_enum");
_Static_assert(sizeof(struct drm_mode_connector_set_property) == 16, "drm_mode_connector_set_property");
_Static_assert(sizeof(struct drm_mode_obj_set_property) == 24, "drm_mode_obj_set_property");
_Static_assert(sizeof(struct drm_event)        == 8,  "drm_event");
_Static_assert(sizeof(struct drm_event_vblank) == 32, "drm_event_vblank");

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

// Import a cross-process dma-buf fd as a mappable dumb-BO. The fd carries the shared
// backing: the exporter's PRIME_HANDLE_TO_FD now returns a dup of its BO's MAP_SHARED
// anon file (see below), so the pixel bytes travel with the fd across SCM_RIGHTS. We
// dup the fd so THIS process owns a reference, mmap it MAP_SHARED as the canonical view,
// and record the (st_dev, st_ino) identity in the export table so MODE_MAP_DUMB /
// drm_mmap_bo (the guest's EGL import: drmPrimeFDToHandle -> drmModeMapDumb -> mmap) and
// drm_bo_map_by_fd (the present path) all resolve to the SAME shared pixels. Returns the
// new handle (>= 0) or a negative errno.
static int bo_import_shared_fd(int fd, dev_t device, ino_t inode) {
  struct bo_slot* slot = NULL;
  for (int i = 0; i < MAX_BO; i++) if (!g_bo[i].used) { slot = &g_bo[i]; break; }
  if (!slot) return -ENOMEM;

  int dup_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
  if (dup_fd < 0) return -EMFILE;

  struct stat st;
  if (fstat(dup_fd, &st) || st.st_size <= 0) { close(dup_fd); return -EINVAL; }

  size_t rlen = (size_t)((st.st_size + 4095) & ~(size_t)4095);
  if (rlen == 0) rlen = 4096;
  /* The imported fd may be read-only (e.g. a mock-gate carrier fd opened
   * O_RDONLY by rd_carrier_issue). mmap(PROT_WRITE) on an O_RDONLY fd fails
   * with EACCES; this function used to map EVERY mmap failure to -ENOMEM, so a
   * 3440x1440 (19.8 MB) carrier import hard-failed with -12 and broke the
   * present content path. Fall back to a read-only canonical view so the fd
   * still imports (valid handle + mmap); the present path only reads this
   * view (drm_bo_map_by_fd). */
  void* m = mmap(NULL, rlen, PROT_READ | PROT_WRITE, MAP_SHARED, dup_fd, 0);
  if (m == MAP_FAILED)
    m = mmap(NULL, rlen, PROT_READ, MAP_SHARED, dup_fd, 0);
  if (m == MAP_FAILED) { close(dup_fd); return -ENOMEM; }

  slot->handle = g_next_bo_handle;
  slot->map = m;
  slot->backing_fd = dup_fd;
  slot->size = (size_t)st.st_size;
  slot->used = 1;
  /* Two references: one for the returned handle (released by the caller's
   * GEM_CLOSE) and one for the dup'd fd (slot->backing_fd), mirroring
   * PRIME_HANDLE_TO_FD's bo_ref() for the exported fd. wlsrv.c GEM_CLOSEs the
   * handle immediately after PRIME_FD_TO_HANDLE (it keeps only dmabuf_fd for
   * drm_bo_map_by_fd); with refs==1 that GEM_CLOSE freed the BO and forgot the
   * export entry, so the later drm_bo_map_by_fd(dmabuf_fd) returned NULL and
   * the present path dropped the frame. */
  slot->refs = 2;
  export_remember(slot->handle, device, inode);
  return (int)g_next_bo_handle++;
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

// ---- framebuffer table: fb_id -> backing BO + geometry (for the DRM flip present hook) ----
// MODE_ADDFB2 wraps a GEM/dumb handle (the BO the sw winsys rendered into) with width, height
// and stride. The atomic/page-flip commit references only the fb_id, so the present hook resolves
// fb_id -> handle -> g_bo[] canonical view and pushes those exact pixels to the Metal window.
#define MAX_FB 64
struct fb_slot {
  uint32_t fb_id;
  uint32_t handle;      // GEM/dumb handle of the backing BO (g_bo[])
  uint32_t width, height;
  uint32_t pitch;       // stride in bytes (may exceed width*4 due to 64-byte dumb padding)
  uint32_t format;      // fourcc (DRM_FORMAT_XRGB8888 == 'XR24')
  int used;
};
static struct fb_slot g_fb[MAX_FB];

static struct fb_slot* fb_find(uint32_t fb_id) {
  for (int i = 0; i < MAX_FB; i++)
    if (g_fb[i].used && g_fb[i].fb_id == fb_id) return &g_fb[i];
  return NULL;
}
static void fb_forget(uint32_t fb_id) {
  struct fb_slot* f = fb_find(fb_id);
  if (f) memset(f, 0, sizeof *f);
}

// DRM flip present hook: push the just-committed framebuffer to the Metal window for a given
// CRTC. Resolves fb_id -> fb_slot -> bo_slot, samples non-black for the trace line, then hands
// the BGRA bytes to gpuwin_present_crtc(bgra=1, crtc_id) so the primary (crtc 1) and secondary
// (crtc 2) outputs each present their own frame. XRGB8888 is already B,G,R,X in memory, so no
// swizzle — only a stride repack when the dumb pitch has padding the staging upload lacks.
static void drm_present_fb(uint32_t fb_id, uint32_t crtc_id) {
  struct fb_slot* f = fb_find(fb_id);
  if (!f || !f->handle) {
    fprintf(stderr, "DRM_PRESENT crtc=%u fb_id=0x%x -> no framebuffer recorded (skip)\n",
            crtc_id, fb_id);
    return;
  }
  struct bo_slot* b = bo_find(f->handle);
  if (!b || !b->map) {
    fprintf(stderr, "DRM_PRESENT crtc=%u fb_id=0x%x handle=0x%x -> no BO backing (skip)\n",
            crtc_id, fb_id, f->handle);
    return;
  }
  uint32_t w = f->width, h = f->height;
  if (!w || !h || w > 16384 || h > 16384) return;

  const uint8_t* px = (const uint8_t*)b->map;
  size_t tight = (size_t)w * 4;

  // Non-black selfcheck: coarse grid over the whole frame, B/G/R channels (X/alpha is unused).
  int nonblack = 0;
  {
    uint32_t sx = w > 1024 ? 97 : 1;
    uint32_t sy = h > 1024 ? 97 : 1;
    for (uint32_t y = 0; y < h && !nonblack; y += sy)
      for (uint32_t x = 0; x < w && !nonblack; x += sx) {
        const uint8_t* p = px + (size_t)y * f->pitch + (size_t)x * 4;
        if (p[0] || p[1] || p[2]) nonblack = 1;
      }
  }

  static unsigned frame;
  unsigned n = ++frame;
  fprintf(stderr, "DRM_PRESENT crtc=%u frame=%u %ux%u %s pitch=%u\n",
          crtc_id, n, w, h, nonblack ? "non-black" : "black", f->pitch);

  if (f->pitch == tight) {
    gpuwin_present_crtc(px, w, h, /*bgra=*/1, crtc_id);   // no row repack; host presenter still copies/uploads
  } else if (f->pitch >= tight) {
    uint8_t* tmp = (uint8_t*)malloc(tight * h);
    if (!tmp) return;
    for (uint32_t y = 0; y < h; y++)
      memcpy(tmp + (size_t)y * tight, px + (size_t)y * f->pitch, tight);
    gpuwin_present_crtc(tmp, w, h, /*bgra=*/1, crtc_id);
    free(tmp);
  }
}

int drm_enabled(void) {
  // Present by default (the rdmetal render node is now the installed default
  // driver, so Hyprland/KWin must see /dev/dri/renderD128 with no env vars).
  // RDDRM=0 explicitly hides the node; RDDRM=1 is a no-op (already on).
  if (g_on < 0) { const char* e = getenv("RDDRM"); g_on = !(e && *e == '0'); }
  return g_on;
}

// "DRM master active": the guest issued a KMS-master ioctl (SET_CLIENT_CAP with
// UNIVERSAL_PLANES/ATOMIC, or MODE_GETRESOURCES). Render-only clients (Mesa on
// renderD128) never do this; aquamarine's native DRM backend does it at init, before
// its nested-Wayland fallback backend ever connects. wlsrv.c reads this to decide
// whether to advertise a wl_output (the nested presentation must stay retired when
// the compositor is already presenting natively as the DRM master).
static atomic_int g_master_active;
int drm_master_active(void) { return atomic_load(&g_master_active); }
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
//   7 = /sys/class/drm                       (S_IFDIR)  libudev scanGPUs entry
//   8 = /sys/class/drm/{card0,renderD128}    (S_IFDIR)
//   9 = /sys/class/drm/<name>/uevent         (S_IFREG)  DRM card uevent
//  10 = /sys/class/drm/<name>/dev            (S_IFREG)  "226:<minor>"
//  11 = /sys/class/drm/<name>/subsystem      (S_IFLNK)  -> /sys/class/drm
int drm_sysfs_kind(const char* path) {
  if (!path) return 0;
  static const char pre[] = "/sys/dev/char/226:";
  if (strncmp(path, pre, sizeof pre - 1)) {
    // /sys/class/drm — the libudev enumeration surface for aquamarine's scanGPUs().
    static const char cls[] = "/sys/class/drm";
    if (strncmp(path, cls, sizeof cls - 1)) return 0;
    const char* p = path + sizeof cls - 1;
    if (*p == 0) return 7;                                   // /sys/class/drm
    if (*p != '/') return 0;
    p++;
    const char* leaf = p;
    while (*p && *p != '/') p++;
    int is_card = (p - leaf == 5 && strncmp(leaf, "card0", 5) == 0);
    int is_rnd  = (p - leaf == 9 && strncmp(leaf, "renderD128", 9) == 0);
    if (!is_card && !is_rnd) return 0;
    if (*p == 0) return 8;                                   // /sys/class/drm/<name>
    if (strcmp(p, "/uevent") == 0)    return 9;
    if (strcmp(p, "/dev") == 0)       return 10;
    if (strcmp(p, "/subsystem") == 0) return 11;
    return 0;
  }
  const char* p = path + sizeof pre - 1;
  // sd-device resolves a character device number through this sysfs symlink.
  // Without it udev's tag index cannot turn c226:0 into a seat device.
  if (strcmp(p, "0") == 0) return 12;
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

// Minor for a /sys/class/drm/<name> path: 0 for card0, 128 for renderD128, -1 otherwise.
static int drm_class_minor(const char* path) {
  const char* p = path ? strstr(path, "/sys/class/drm/") : NULL;
  if (!p) return -1;
  p += sizeof("/sys/class/drm/") - 1;
  if (strncmp(p, "card0", 5) == 0) return 0;
  if (strncmp(p, "renderD128", 9) == 0) return 128;
  return -1;
}

// Content served for /sys/dev/char/226:*/uevent. libdrm reads DEVNAME and prepends "/dev/",
// so the value must be the path relative to /dev ("dri/card0") for the returned name to be an
// openable devnode (aquamarine reopens it with open(name) in reopenDRMNode()).
const char* drm_sysfs_uevent(void) { return "DEVNAME=dri/card0\n"; }

// Full DRM uevent body for /sys/class/drm/<name>/uevent (libudev scanGPUs reads this).
const char* drm_sysfs_uevent_for(const char* path) {
  int m = drm_class_minor(path);
  static char b[160];
  snprintf(b, sizeof b,
           "MAJOR=226\nMINOR=%d\nDEVNAME=dri/%s\nDEVTYPE=drm_minor\nSUBSYSTEM=drm\n",
           m, m == 128 ? "renderD128" : "card0");
  return b;
}

// dev file body ("226:<minor>") for /sys/class/drm/<name>/dev.
const char* drm_sysfs_dev_for(const char* path) {
  int m = drm_class_minor(path);
  static char b[16];
  snprintf(b, sizeof b, "226:%d\n", m);
  return b;
}

// Target reported for /sys/dev/char/226:*/device/subsystem. libdrm's drmParseSubsystemType
// readlink()s this and maps the trailing path component to a bus type ("/pci" -> PCI), so the
// synthetic device enumerates as a PCI GPU. (Apple's AGX is PCIe, matching the "asahi" name.)
const char* drm_sysfs_link(void) { return "/pci"; }

// Target for /sys/class/drm/<name>/subsystem (libudev subsystem probe -> "drm").
const char* drm_sysfs_class_link(void) { return "/sys/class/drm"; }

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
  if (k == 2 || k == 5 || k == 6 || k == 9 || k == 10) { *mode = 0100000u; *rdev = 0; return; } // S_IFREG
  if (k == 3 || k == 7 || k == 8) { *mode = 0040000u; *rdev = 0; return; }                    // S_IFDIR
  if (k == 4 || k == 11 || k == 12) { *mode = 0120000u; *rdev = 0; return; }                  // S_IFLNK
  int render = path && strstr(path, "renderD") != NULL;
  *mode = (*mode & 0777u) | 0020000u;                       // S_IFCHR
  *rdev = ((unsigned long)DRM_MAJOR << 8) | (render ? 128u : 0u);
}

// A preferred mode at an arbitrary native size (reduced-blanking timings). The shim
// never drives a real CRTC, so the exact blanking is cosmetic; aquamarine only needs
// hdisplay/vdisplay/vrefresh to be the real display's native pixels so it modesets
// the guest output at full resolution.
static void fill_mode_wh(struct drm_mode_modeinfo* m, uint16_t hd, uint16_t vd,
                         uint32_t refresh, const char* name) {
  memset(m, 0, sizeof *m);
  uint16_t htotal = (uint16_t)(hd + 80);
  uint16_t vtotal = (uint16_t)(vd + 46);
  m->clock       = (uint32_t)((uint64_t)htotal * (uint64_t)vtotal * refresh / 1000);
  m->hdisplay    = hd; m->hsync_start = (uint16_t)(hd + 16); m->hsync_end = (uint16_t)(hd + 48);
  m->htotal      = htotal; m->hskew = 0;
  m->vdisplay    = vd; m->vsync_start = (uint16_t)(vd + 3); m->vsync_end = (uint16_t)(vd + 9);
  m->vtotal      = vtotal; m->vscan = 0;
  m->vrefresh    = refresh;
  m->flags       = 0;
  m->type        = DRM_MODE_TYPE_PREFERRED;
  snprintf(m->name, sizeof m->name, "%s", name);
}

// The two physical displays the DRM node advertises:
//   connector 1 = external 3440x1440 (DisplayPort)
//   connector 2 = builtin  3600x2338 (eDP, Retina panel)
// The CLI has no AppKit output inventory. Its weak symbol is absent, so retain
// the historical diagnostic mode there. The GUI uses actual connected display
// geometry; an unplugged external connector has no advertised mode.
extern int rdgpu_drm_connector_output(uint32_t, uint32_t*, uint32_t*) __attribute__((weak_import));
static int fill_mode_for_connector(uint32_t connector_id, struct drm_mode_modeinfo* m) {
  uint32_t width = 0, height = 0;
  if (rdgpu_drm_connector_output) {
    if (rdgpu_drm_connector_output(connector_id, &width, &height) != 0) return -1;
  } else if (connector_id == 2) {
    width = 3600; height = 2338;
  } else {
    width = 3440; height = 1440;
  }
  if (!width || !height || width > UINT16_MAX - 80 || height > UINT16_MAX - 46) return -1;
  char name[32];
  snprintf(name, sizeof name, "%ux%u", width, height);
  fill_mode_wh(m, (uint16_t)width, (uint16_t)height, 60, name);
  return 0;
}

// ---- KMS state (one CRTC, one connector, one primary plane, id 1 each) -------------
// The compositor's DRM backend does two kinds of commit: a MODESET (MODE_SETCRTC, or
// MODE_ATOMIC with ALLOW_MODESET + a MODE_ID blob) and a FLIP (MODE_PAGE_FLIP, or
// MODE_ATOMIC with a new FB_ID + PAGE_FLIP_EVENT). This tracks both so MODE_GETCRTC
// and MODE_OBJ_GETPROPERTIES report the current state honestly.
struct crtc_state {
  uint32_t fb_id;
  int32_t x, y;
  int mode_valid;
  uint32_t active;                       // CRTC ACTIVE property
  uint32_t mode_blob;                    // CRTC MODE_ID property (blob id, 0 = none)
  struct drm_mode_modeinfo mode;
};
static struct crtc_state g_crtc;
// Second display (builtin). Discovery advertises TWO CRTC/connector/encoder/plane
// sets (external + builtin); the state for the second lives beside the first so the
// single-display commit handlers keep their g_crtc/g_connector/g_plane references
// unchanged while the discovery ioctls index by object id.
static struct crtc_state g_crtc2;

struct plane_state {
  uint32_t fb_id;
  uint32_t crtc_id;
  uint64_t src_x, src_y, src_w, src_h;   // 16.16 fixed
  uint64_t crtc_x, crtc_y, crtc_w, crtc_h;
};
static struct plane_state g_plane = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static struct plane_state g_plane2 = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

struct connector_state {
  uint32_t crtc_id;
  uint32_t dpms;                         // DRM_MODE_DPMS_*
};
static struct connector_state g_connector;
static struct connector_state g_connector2;

// Per-object state accessors. Object ids 1 and 2 map to the primary (external) and
// secondary (builtin) CRTC/connector/plane respectively.
static struct crtc_state* crtc_by_id(uint32_t id)       { return id == 2 ? &g_crtc2 : &g_crtc; }
static struct plane_state* plane_by_id(uint32_t id)     { return id == 2 ? &g_plane2 : &g_plane; }
static struct connector_state* connector_by_id(uint32_t id) { return id == 2 ? &g_connector2 : &g_connector; }

// Property blobs (MODE_ID modeinfo, and anything else the compositor creates).
// drmModeCreatePropertyBlob stores arbitrary bytes keyed by a monotonic id; the atomic
// commit references MODE_ID by that id, and getCurrentMode() reads it back via
// MODE_GETPROPBLOB.
#define MAX_BLOBS 64
struct blob_slot {
  uint32_t id;
  uint32_t len;
  unsigned char data[128];
  int used;
};
static struct blob_slot g_blobs[MAX_BLOBS];
static uint32_t g_next_blob = 1;

static struct blob_slot* blob_find(uint32_t id) {
  for (int i = 0; i < MAX_BLOBS; i++)
    if (g_blobs[i].used && g_blobs[i].id == id) return &g_blobs[i];
  return NULL;
}
static uint32_t blob_create(const void* data, uint32_t len) {
  if (!data || !len || len > sizeof(((struct blob_slot*)0)->data)) return 0;
  struct blob_slot* s = NULL;
  for (int i = 0; i < MAX_BLOBS; i++) if (!g_blobs[i].used) { s = &g_blobs[i]; break; }
  if (!s) return 0;
  memset(s, 0, sizeof *s);
  s->id = g_next_blob++; if (!g_next_blob) g_next_blob = 1;
  s->len = len;
  memcpy(s->data, data, len);
  s->used = 1;
  return s->id;
}
static void blob_destroy(uint32_t id) {
  struct blob_slot* s = blob_find(id);
  if (s) memset(s, 0, sizeof *s);
}

// Page-flip / atomic-completion events. The shim never waits for a real vblank; a flip
// is "complete" immediately, so we write a DRM_EVENT_FLIP_COMPLETE into the event pipe on
// every commit carrying DRM_MODE_PAGE_FLIP_EVENT. Every DRM fd handed to the guest is a
// dup of g_ev_read_fd, so read()/poll() on it behave like a real DRM fd: readable only
// while an event is pending, and read() returns the event bytes. aquamarine's async
// commit thread consumes these via drmHandleEvent().
static int g_ev_read_fd = -1, g_ev_write_fd = -1;
static int g_flip_seq;

// Create the event pipe lazily and hand the guest a dup of the read end.
int drm_open_fd(void) {
  if (g_ev_read_fd < 0) {
    int p[2];
    if (pipe(p) != 0) return -1;
    g_ev_read_fd = p[0]; g_ev_write_fd = p[1];
    fcntl(g_ev_read_fd, F_SETFD, FD_CLOEXEC);
    fcntl(g_ev_write_fd, F_SETFD, FD_CLOEXEC);
  }
  return fcntl(g_ev_read_fd, F_DUPFD_CLOEXEC, 0);
}

static void event_enqueue(uint64_t user_data, uint32_t crtc_id) {
  if (g_ev_write_fd < 0) return;
  struct drm_event_vblank e;
  memset(&e, 0, sizeof e);
  e.base.type   = DRM_EVENT_FLIP_COMPLETE;
  e.base.length = sizeof e;
  e.user_data   = user_data;
  e.tv_sec      = 0; e.tv_usec = 0;
  e.sequence    = (uint32_t)++g_flip_seq;
  e.crtc_id     = crtc_id ? crtc_id : 1;
  // Best-effort: a full pipe (the compositor stopped draining) is not fatal.
  ssize_t r = write(g_ev_write_fd, &e, sizeof e);
  (void)r;
}

// Property table: name, flags, and (for enum props) the enum values. aquamarine matches
// by NAME (Props.cpp scanProperties), so the id assignment only needs to be internally
// consistent between MODE_OBJ_GETPROPERTIES, MODE_GETPROPERTY and MODE_ATOMIC.
struct prop_enum { uint64_t value; const char* name; };
struct prop_def {
  uint32_t id;
  const char* name;
  uint32_t flags;
  const struct prop_enum* enums;
  int num_enums;
};
static const struct prop_enum kPlaneTypeEnums[] = {
  { DRM_PLANE_TYPE_OVERLAY, "Overlay" }, { DRM_PLANE_TYPE_PRIMARY, "Primary" },
  { DRM_PLANE_TYPE_CURSOR, "Cursor" },
};
static const struct prop_enum kDpmsEnums[] = {
  { 0, "On" }, { 1, "Standby" }, { 2, "Suspend" }, { 3, "Off" },
};
static const struct prop_def kProps[] = {
  { DRM_PROP_TYPE,    "type",    DRM_MODE_PROP_ENUM | DRM_MODE_PROP_IMMUTABLE | DRM_MODE_PROP_ATOMIC, kPlaneTypeEnums, 3 },
  { DRM_PROP_ACTIVE,  "ACTIVE",  DRM_MODE_PROP_RANGE | DRM_MODE_PROP_ATOMIC, NULL, 0 },
  { DRM_PROP_MODE_ID, "MODE_ID", DRM_MODE_PROP_BLOB  | DRM_MODE_PROP_ATOMIC, NULL, 0 },
  { DRM_PROP_CRTC_ID, "CRTC_ID", DRM_MODE_PROP_OBJECT | DRM_MODE_PROP_ATOMIC, NULL, 0 },
  { DRM_PROP_FB_ID,   "FB_ID",   DRM_MODE_PROP_OBJECT | DRM_MODE_PROP_ATOMIC, NULL, 0 },
  { DRM_PROP_SRC_X,   "SRC_X",   DRM_MODE_PROP_RANGE | DRM_MODE_PROP_ATOMIC, NULL, 0 },
  { DRM_PROP_SRC_Y,   "SRC_Y",   DRM_MODE_PROP_RANGE | DRM_MODE_PROP_ATOMIC, NULL, 0 },
  { DRM_PROP_SRC_W,   "SRC_W",   DRM_MODE_PROP_RANGE | DRM_MODE_PROP_ATOMIC, NULL, 0 },
  { DRM_PROP_SRC_H,   "SRC_H",   DRM_MODE_PROP_RANGE | DRM_MODE_PROP_ATOMIC, NULL, 0 },
  { DRM_PROP_CRTC_X,  "CRTC_X",  DRM_MODE_PROP_RANGE | DRM_MODE_PROP_ATOMIC, NULL, 0 },
  { DRM_PROP_CRTC_Y,  "CRTC_Y",  DRM_MODE_PROP_RANGE | DRM_MODE_PROP_ATOMIC, NULL, 0 },
  { DRM_PROP_CRTC_W,  "CRTC_W",  DRM_MODE_PROP_RANGE | DRM_MODE_PROP_ATOMIC, NULL, 0 },
  { DRM_PROP_CRTC_H,  "CRTC_H",  DRM_MODE_PROP_RANGE | DRM_MODE_PROP_ATOMIC, NULL, 0 },
  { DRM_PROP_DPMS,    "DPMS",    DRM_MODE_PROP_ENUM | DRM_MODE_PROP_ATOMIC, kDpmsEnums, 4 },
};
#define NUM_PROPS (sizeof kProps / sizeof kProps[0])

static const struct prop_def* prop_find_by_id(uint32_t id) {
  for (size_t i = 0; i < NUM_PROPS; i++) if (kProps[i].id == id) return &kProps[i];
  return NULL;
}

// Per-object property attachment (which prop ids each object exposes, in order).
static const uint32_t kCrtcProps[]    = { DRM_PROP_ACTIVE, DRM_PROP_MODE_ID };
static const uint32_t kConnectorProps[] = { DRM_PROP_CRTC_ID, DRM_PROP_DPMS };
static const uint32_t kPlaneProps[]   = { DRM_PROP_TYPE, DRM_PROP_FB_ID, DRM_PROP_CRTC_ID,
                                          DRM_PROP_SRC_X, DRM_PROP_SRC_Y, DRM_PROP_SRC_W, DRM_PROP_SRC_H,
                                          DRM_PROP_CRTC_X, DRM_PROP_CRTC_Y, DRM_PROP_CRTC_W, DRM_PROP_CRTC_H };

// Current value of a property on a given object (for MODE_OBJ_GETPROPERTIES and
// MODE_GETCONNECTOR). `obj_id` selects the CRTC/connector/plane (1 or 2).
static uint64_t prop_value(uint32_t obj_type, uint32_t obj_id, uint32_t prop_id) {
  struct crtc_state* cr = crtc_by_id(obj_id);
  struct plane_state* pl = plane_by_id(obj_id);
  struct connector_state* cn = connector_by_id(obj_id);
  switch (prop_id) {
    case DRM_PROP_TYPE:    return DRM_PLANE_TYPE_PRIMARY;
    case DRM_PROP_ACTIVE:  return cr->active;
    case DRM_PROP_MODE_ID: return cr->mode_blob;
    case DRM_PROP_CRTC_ID: return (obj_type == DRM_MODE_OBJECT_CONNECTOR) ? cn->crtc_id : pl->crtc_id;
    case DRM_PROP_FB_ID:   return pl->fb_id;
    case DRM_PROP_SRC_X:   return pl->src_x;
    case DRM_PROP_SRC_Y:   return pl->src_y;
    case DRM_PROP_SRC_W:   return pl->src_w;
    case DRM_PROP_SRC_H:   return pl->src_h;
    case DRM_PROP_CRTC_X:  return pl->crtc_x;
    case DRM_PROP_CRTC_Y:  return pl->crtc_y;
    case DRM_PROP_CRTC_W:  return pl->crtc_w;
    case DRM_PROP_CRTC_H:  return pl->crtc_h;
    case DRM_PROP_DPMS:    return cn->dpms;
    default:               return 0;
  }
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
  // here IS the BO's MAP_SHARED anon backing file (b->backing_fd), dup'd so the receiver owns a
  // reference. This makes the pixel bytes travel WITH the fd across SCM_RIGHTS: a DIFFERENT
  // process (kwin importing plasmashell's buffer) can mmap the received fd and read the exact
  // frame, instead of resolving an identity-only carrier it has no backing for. The (st_dev,
  // st_ino) identity of that backing is recorded against the handle, so the in-process importer
  // (export_find) and the present path (drm_bo_map_by_fd) still resolve it unchanged.
  if (type == DRM_TYPE && nr == DRM_NR_PRIME_HANDLE_TO_FD && arg) {
    struct drm_prime_handle* ph = (struct drm_prime_handle*)arg;
    ph->fd = -1;
    struct bo_slot* b = bo_find(ph->handle);
    if (!b || b->backing_fd < 0) {
      if (tracing())
        fprintf(stderr, "[drm] PRIME_HANDLE_TO_FD handle=0x%x -> -EINVAL (unknown)\n", ph->handle);
      return -22;
    }
    // Honor DRM_CLOEXEC (the caller requested it): the SCM_RIGHTS receiver re-marks it anyway.
    int cfd = fcntl(b->backing_fd, F_DUPFD_CLOEXEC, 0);
    if (cfd < 0) return -12;
    struct stat st;
    if (fstat(cfd, &st) || st.st_dev == 0 || st.st_ino == 0) { close(cfd); return -12; }
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
  // guest dmabuf fd into an opaque GEM handle. Two cases: (1) the fd's (st_dev, st_ino) is
  // already recorded in g_export[] — an in-process "own export" round-trip OR a repeated import
  // of a cross-process fd we already admitted — return the recorded handle (the sw winsys's
  // find_and_ref by handle reuses the SAME BO); (2) a fresh cross-process fd carries the
  // exporter's shared backing (PRIME_HANDLE_TO_FD returns the BO's MAP_SHARED anon file), so we
  // import it as a mappable dumb-BO. The guest's EGL then does drmModeMapDumb + mmap, which now
  // succeed (no -EINVAL -> null deref in rdmetal_dri.so), and the present path reads the pixels.
  // The mach-port IOSurface handoff stays for the Metal/scanout path (wlsrv.c mac_surface_*,
  // rdgpu_handoff_resolve_fd directly) — it is not needed for a CPU (softpipe) dumb buffer.
  if (type == DRM_TYPE && nr == DRM_NR_PRIME_FD_TO_HANDLE && arg) {
    struct drm_prime_handle* ph = (struct drm_prime_handle*)arg;

    struct stat st;
    int have_id = !fstat(ph->fd, &st) && st.st_dev && st.st_ino;
    if (!have_id) return -EINVAL;          // cannot import an fd with no stable identity

    if (export_find(st.st_dev, st.st_ino)) {
      // Own export or repeated import: the backing BO is already in this process. Return
      // the ORIGINAL handle (not a fresh minted one) so the sw winsys's import (find_and_ref
      // by handle) reuses the SAME BO and softpipe reads the SAME host backing.
      struct export_slot* e = export_find(st.st_dev, st.st_ino);
      struct bo_slot* b = bo_find(e->handle);
      ph->handle = e->handle;
      if (b) bo_ref(b);                     // importer holds a reference
      fprintf(stderr, "DRM_IMPORT_PRIME fd=%d -> handle=0x%x (own export)\n", ph->fd, ph->handle);
      return 0;
    }

    // Fresh cross-process import: map the shared backing the fd carries.
    int h = bo_import_shared_fd(ph->fd, st.st_dev, st.st_ino);
    if (h < 0) {
      fprintf(stderr, "DRM_IMPORT_CROSS_PROCESS fd=%d dev=%d ino=%llu import_failed=%d\n",
              ph->fd, (int)st.st_dev, (unsigned long long)st.st_ino, h);
      return h;
    }
    ph->handle = (uint32_t)h;
    fprintf(stderr, "DRM_IMPORT_CROSS_PROCESS fd=%d dev=%d ino=%llu -> handle=0x%x size=%zu\n",
            ph->fd, (int)st.st_dev, (unsigned long long)st.st_ino, ph->handle,
            bo_find(ph->handle) ? bo_find(ph->handle)->size : (size_t)0);
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
    if (ok) atomic_store(&g_master_active, 1);   // KMS master is negotiating the atomic path
    if (tracing())
      fprintf(stderr, "[drm] SET_CLIENT_CAP cap=%llu value=%llu -> %s\n",
              (unsigned long long)sc->capability, (unsigned long long)sc->value,
              ok ? "ok" : "-EINVAL");
    return ok ? 0 : -22;
  }

  // MODE_GETRESOURCES: TWO CRTCs/connectors/encoders (external + builtin). The kernel
  // two-call protocol: first call has NULL id pointers (counts only), the second has
  // arrays we fill. Answering both with the same counts keeps it idempotent.
  if (type == DRM_TYPE && nr == DRM_NR_MODE_GETRESOURCES && arg) {
    struct drm_mode_card_res* r = (struct drm_mode_card_res*)arg;
    r->count_fbs = 1;
    r->count_crtcs = 2;
    r->count_connectors = 2;
    r->count_encoders = 2;
    r->min_width = 1; r->max_width = 16384;
    r->min_height = 1; r->max_height = 16384;
    if (r->fb_id_ptr)        { ((uint32_t*)(uintptr_t)r->fb_id_ptr)[0] = 1; }
    if (r->crtc_id_ptr)      { ((uint32_t*)(uintptr_t)r->crtc_id_ptr)[0] = 1; ((uint32_t*)(uintptr_t)r->crtc_id_ptr)[1] = 2; }
    if (r->connector_id_ptr) { ((uint32_t*)(uintptr_t)r->connector_id_ptr)[0] = 1; ((uint32_t*)(uintptr_t)r->connector_id_ptr)[1] = 2; }
    if (r->encoder_id_ptr)   { ((uint32_t*)(uintptr_t)r->encoder_id_ptr)[0] = 1; ((uint32_t*)(uintptr_t)r->encoder_id_ptr)[1] = 2; }
    atomic_store(&g_master_active, 1);
    if (tracing())
      fprintf(stderr, "[drm] MODE_GETRESOURCES fbs=%u crtcs=%u connectors=%u encoders=%u\n",
              r->count_fbs, r->count_crtcs, r->count_connectors, r->count_encoders);
    return 0;
  }

  // MODE_GETCRTC: report the current CRTC state (mode_valid=0 until the first modeset).
  if (type == DRM_TYPE && nr == DRM_NR_MODE_GETCRTC && arg) {
    struct drm_mode_get_crtc* c = (struct drm_mode_get_crtc*)arg;
    struct crtc_state* cr = crtc_by_id(c->crtc_id);
    c->set_connectors_ptr = 0;
    c->count_connectors = 0;
    c->fb_id = cr->fb_id;
    c->x = cr->x; c->y = cr->y;
    c->gamma_size = 0;
    c->mode_valid = cr->mode_valid;
    c->mode = cr->mode_valid ? cr->mode : (struct drm_mode_modeinfo){0};
    if (tracing())
      fprintf(stderr, "[drm] MODE_GETCRTC crtc_id=%u mode_valid=%u fb_id=%u%s\n",
              c->crtc_id, c->mode_valid, c->fb_id,
              c->mode_valid ? "" : " (no active mode)");
    return 0;
  }

  // MODE_GETENCODER: two virtual encoders, one per CRTC.
  if (type == DRM_TYPE && nr == DRM_NR_MODE_GETENCODER && arg) {
    struct drm_mode_get_encoder* e = (struct drm_mode_get_encoder*)arg;
    uint32_t crtc = (e->encoder_id == 2) ? 2 : 1;
    e->encoder_type = DRM_MODE_ENCODER_VIRTUAL;
    e->crtc_id = crtc;
    e->possible_crtcs = (crtc == 2) ? 2 : 1;   // bit0 -> crtc1, bit1 -> crtc2
    e->possible_clones = 0;
    if (tracing())
      fprintf(stderr, "[drm] MODE_GETENCODER encoder_id=%u type=VIRTUAL crtc_id=%u\n",
              e->encoder_id, e->crtc_id);
    return 0;
  }

  // MODE_GETCONNECTOR: the external DP and builtin eDP each expose their
  // current host dimensions. An absent display remains a disconnected object.
  if (type == DRM_TYPE && nr == DRM_NR_MODE_GETCONNECTOR && arg) {
    struct drm_mode_get_connector* c = (struct drm_mode_get_connector*)arg;
    uint32_t cid = c->connector_id;
    int is_builtin = (cid == 2);
    struct drm_mode_modeinfo mode;
    int connected = fill_mode_for_connector(cid, &mode) == 0;
    c->encoder_id = is_builtin ? 2 : 1;
    c->connector_type = is_builtin ? DRM_MODE_CONNECTOR_eDP : DRM_MODE_CONNECTOR_DisplayPort;
    c->connector_type_id = 1;
    c->connection = connected ? DRM_MODE_CONNECTED : DRM_MODE_DISCONNECTED;
    c->mm_width = is_builtin ? 300 : 800; c->mm_height = is_builtin ? 200 : 340;
    c->subpixel = 0;
    c->pad = 0;
    c->count_modes = connected ? 1 : 0;
    c->count_props = (uint32_t)(sizeof kConnectorProps / sizeof kConnectorProps[0]);
    c->count_encoders = 1;
    if (c->encoders_ptr) *(uint32_t*)(uintptr_t)c->encoders_ptr = is_builtin ? 2 : 1;
    if (c->modes_ptr && connected)
      *(struct drm_mode_modeinfo*)(uintptr_t)c->modes_ptr = mode;
    if (c->props_ptr)
      for (uint32_t i = 0; i < c->count_props; i++)
        ((uint32_t*)(uintptr_t)c->props_ptr)[i] = kConnectorProps[i];
    if (c->prop_values_ptr)
      for (uint32_t i = 0; i < c->count_props; i++)
        ((uint64_t*)(uintptr_t)c->prop_values_ptr)[i] =
          prop_value(DRM_MODE_OBJECT_CONNECTOR, cid, kConnectorProps[i]);
    if (tracing())
      fprintf(stderr, "[drm] MODE_GETCONNECTOR connector_id=%u type=%s connection=%u modes=%u encoders=%u props=%u\n",
              c->connector_id, is_builtin ? "eDP" : "DP", c->connection,
              c->count_modes, c->count_encoders, c->count_props);
    return 0;
  }

  // MODE_GETPLANERESOURCES: two primary planes (one per CRTC).
  if (type == DRM_TYPE && nr == DRM_NR_MODE_GETPLANERESOURCES && arg) {
    struct drm_mode_get_plane_res* p = (struct drm_mode_get_plane_res*)arg;
    p->count_planes = 2;
    if (p->plane_id_ptr) {
      ((uint32_t*)(uintptr_t)p->plane_id_ptr)[0] = 1;
      ((uint32_t*)(uintptr_t)p->plane_id_ptr)[1] = 2;
    }
    if (tracing())
      fprintf(stderr, "[drm] MODE_GETPLANERESOURCES planes=%u\n", p->count_planes);
    return 0;
  }

  // MODE_ADDFB2 / MODE_RMFB: mint and accept fb ids (the reference fixture has
  // these; ADDFB2 is exercised by aquamarine's buffer path after checkFeatures).
  // ADDFB2 also records fb_id -> {handle, geometry, stride} so the flip present
  // hook can resolve the commit's fb_id back to the dumb buffer's host pixels.
  if (type == DRM_TYPE && nr == DRM_NR_MODE_ADDFB2 && arg) {
    struct drm_mode_fb_cmd2* fb = (struct drm_mode_fb_cmd2*)arg;
    uint32_t old_id = fb->fb_id;
    fb->fb_id = g_next_fb++;
    if (g_next_fb == 0) g_next_fb = 1;
    struct fb_slot* f = NULL;
    for (int i = 0; i < MAX_FB; i++) if (!g_fb[i].used) { f = &g_fb[i]; break; }
    if (f) {
      f->fb_id  = fb->fb_id;
      f->handle = fb->handles[0];
      f->width  = fb->width;
      f->height = fb->height;
      f->pitch  = fb->pitches[0] ? fb->pitches[0] : fb->width * 4;
      f->format = fb->pixel_format;
      f->used   = 1;
    }
    if (tracing())
      fprintf(stderr, "[drm] MODE_ADDFB2 fb_id=%u (was %u) handle=0x%x %ux%u pitch=%u fmt=0x%x\n",
              fb->fb_id, old_id, fb->handles[0], fb->width, fb->height,
              f ? f->pitch : 0, fb->pixel_format);
    return 0;
  }

  if (type == DRM_TYPE && nr == DRM_NR_MODE_RMFB && arg) {
    uint32_t fb_id = *(uint32_t*)arg;
    if (tracing())
      fprintf(stderr, "[drm] MODE_RMFB fb_id=%u\n", fb_id);
    fb_forget(fb_id);
    return 0;
  }

  // MODE_GETPLANE: two primary planes (id 1/2), one per CRTC, exposing the
  // XRGB8888 format the ADDFB2 path accepts.
  if (type == DRM_TYPE && nr == DRM_NR_MODE_GETPLANE && arg) {
    struct drm_mode_get_plane* pl = (struct drm_mode_get_plane*)arg;
    uint32_t crtc = (pl->plane_id == 2) ? 2 : 1;
    pl->crtc_id = crtc;
    pl->fb_id = plane_by_id(pl->plane_id)->fb_id;
    pl->possible_crtcs = (crtc == 2) ? 2 : 1;   // bit0 -> crtc1, bit1 -> crtc2
    pl->gamma_size = 0;
    pl->count_format_types = 1;
    if (pl->format_type_ptr)
      *(uint32_t*)(uintptr_t)pl->format_type_ptr = DRM_FORMAT_XRGB8888;
    if (tracing())
      fprintf(stderr, "[drm] MODE_GETPLANE plane_id=%u crtc_id=%u formats=%u\n",
              pl->plane_id, pl->crtc_id, pl->count_format_types);
    return 0;
  }

  // MODE_OBJ_GETPROPERTIES: return each object's attached properties + current values.
  if (type == DRM_TYPE && nr == DRM_NR_MODE_OBJ_GETPROPERTIES && arg) {
    struct drm_mode_obj_get_properties* o = (struct drm_mode_obj_get_properties*)arg;
    const uint32_t* props = NULL;
    uint32_t n = 0;
    if (o->obj_type == DRM_MODE_OBJECT_CRTC)      { props = kCrtcProps;      n = (uint32_t)(sizeof kCrtcProps / sizeof kCrtcProps[0]); }
    else if (o->obj_type == DRM_MODE_OBJECT_CONNECTOR) { props = kConnectorProps; n = (uint32_t)(sizeof kConnectorProps / sizeof kConnectorProps[0]); }
    else if (o->obj_type == DRM_MODE_OBJECT_PLANE || o->obj_type == DRM_MODE_OBJECT_ANY) { props = kPlaneProps; n = (uint32_t)(sizeof kPlaneProps / sizeof kPlaneProps[0]); }
    o->count_props = n;
    if (o->props_ptr)
      for (uint32_t i = 0; i < n; i++) ((uint32_t*)(uintptr_t)o->props_ptr)[i] = props[i];
    if (o->prop_values_ptr)
      for (uint32_t i = 0; i < n; i++)
        ((uint64_t*)(uintptr_t)o->prop_values_ptr)[i] = prop_value(o->obj_type, o->obj_id, props[i]);
    if (tracing())
      fprintf(stderr, "[drm] MODE_OBJ_GETPROPERTIES obj_id=%u obj_type=0x%x count=%u\n",
              o->obj_id, o->obj_type, o->count_props);
    return 0;
  }

  // MODE_GETPROPERTY: name/flags/values/enums for a property (the atomic property model).
  if (type == DRM_TYPE && nr == DRM_NR_MODE_GETPROPERTY && arg) {
    struct drm_mode_get_property* p = (struct drm_mode_get_property*)arg;
    const struct prop_def* d = prop_find_by_id(p->prop_id);
    if (!d) {
      if (tracing()) fprintf(stderr, "[drm] MODE_GETPROPERTY prop_id=%u -> -EINVAL\n", p->prop_id);
      return -22;
    }
    p->flags = d->flags;
    memset(p->name, 0, sizeof p->name);
    memcpy(p->name, d->name, strlen(d->name));
    p->count_values = 0;
    p->count_enum_blobs = 0;
    if (d->flags & DRM_MODE_PROP_RANGE) {
      p->count_values = 2;
      if (p->values_ptr) {
        ((uint64_t*)(uintptr_t)p->values_ptr)[0] = 0;
        ((uint64_t*)(uintptr_t)p->values_ptr)[1] = UINT64_MAX;
      }
    } else if (d->flags & DRM_MODE_PROP_ENUM) {
      p->count_enum_blobs = d->num_enums;
      if (p->enum_blob_ptr) {
        struct drm_mode_property_enum* e = (struct drm_mode_property_enum*)(uintptr_t)p->enum_blob_ptr;
        for (int i = 0; i < d->num_enums; i++) {
          e[i].value = d->enums[i].value;
          memset(e[i].name, 0, sizeof e[i].name);
          memcpy(e[i].name, d->enums[i].name, strlen(d->enums[i].name));
        }
      }
    }
    if (tracing())
      fprintf(stderr, "[drm] MODE_GETPROPERTY prop_id=%u -> \"%s\" flags=0x%x enums=%u values=%u\n",
              p->prop_id, d->name, d->flags, p->count_enum_blobs, p->count_values);
    return 0;
  }

  // MODE_GETPROPBLOB: return stored blob bytes (MODE_ID modeinfo, IN_FORMATS, EDID).
  if (type == DRM_TYPE && nr == DRM_NR_MODE_GETPROPBLOB && arg) {
    struct drm_mode_get_blob* b = (struct drm_mode_get_blob*)arg;
    struct blob_slot* s = blob_find(b->blob_id);
    b->length = s ? s->len : 0;
    if (s && b->data && b->length)
      memcpy((void*)(uintptr_t)b->data, s->data, s->len);
    if (tracing())
      fprintf(stderr, "[drm] MODE_GETPROPBLOB blob_id=%u -> %u bytes%s\n",
              b->blob_id, b->length, s ? "" : " (unknown)");
    return 0;
  }

  // ---- KMS commit path: legacy (MODE_SETCRTC + MODE_PAGE_FLIP) and atomic (MODE_ATOMIC) ----

  // MODE_CREATEPROPBLOB: store arbitrary bytes (usually a drm_mode_modeinfo) and return
  // a blob id. The atomic commit references the MODE_ID blob by this id.
  if (type == DRM_TYPE && nr == DRM_NR_MODE_CREATEPROPBLOB && arg) {
    struct drm_mode_create_blob* cb = (struct drm_mode_create_blob*)arg;
    cb->blob_id = blob_create((const void*)(uintptr_t)cb->data, cb->length);
    if (tracing())
      fprintf(stderr, "[drm] MODE_CREATEPROPBLOB len=%u -> blob_id=%u%s\n",
              cb->length, cb->blob_id, cb->blob_id ? "" : " (-ENOMEM)");
    return cb->blob_id ? 0 : -12;
  }

  if (type == DRM_TYPE && nr == DRM_NR_MODE_DESTROYPROPBLOB && arg) {
    struct drm_mode_destroy_blob* db = (struct drm_mode_destroy_blob*)arg;
    if (tracing()) fprintf(stderr, "[drm] MODE_DESTROYPROPBLOB blob_id=%u\n", db->blob_id);
    blob_destroy(db->blob_id);
    return 0;
  }

  // MODE_SETCRTC (legacy modeset): drmModeSetCrtc(fd, crtc, fb, x, y, connectors, n, mode).
  // The struct is drm_mode_crtc with the mode + connectors as INPUT; fb_id == 0xFFFFFFFF
  // disables scanout.
  if (type == DRM_TYPE && nr == DRM_NR_MODE_SETCRTC && arg) {
    struct drm_mode_get_crtc* c = (struct drm_mode_get_crtc*)arg;
    int disable = (c->fb_id == 0 || c->fb_id == 0xFFFFFFFFu);
    uint32_t crtc = c->crtc_id ? c->crtc_id : 1;
    struct crtc_state* cr = crtc_by_id(crtc);
    struct plane_state* pl = plane_by_id(crtc);
    struct connector_state* cn = connector_by_id(crtc);
    cr->fb_id = disable ? 0 : c->fb_id;
    cr->x = c->x; cr->y = c->y;
    pl->fb_id = cr->fb_id;
    pl->crtc_id = disable ? 0 : crtc;
    cn->crtc_id = disable ? 0 : crtc;
    if (!disable && c->mode.clock) { cr->mode = c->mode; cr->mode_valid = 1; }
    else if (disable) { cr->mode_valid = 0; memset(&cr->mode, 0, sizeof cr->mode); }
    if (tracing())
      fprintf(stderr, "[drm] MODE_SETCRTC crtc_id=%u fb_id=0x%x x=%u y=%u connectors=%u %s "
              "mode=%ux%u@%u -> %s\n",
              c->crtc_id, c->fb_id, c->x, c->y, c->count_connectors,
              disable ? "DISABLE" : "ENABLE", cr->mode_valid ? cr->mode.hdisplay : 0,
              cr->mode_valid ? cr->mode.vdisplay : 0, cr->mode_valid ? cr->mode.vrefresh : 0,
              "ok");
    return 0;
  }

  // MODE_SETPROPERTY (legacy drmModeConnectorSetProperty, e.g. DPMS): accept + record.
  if (type == DRM_TYPE && nr == DRM_NR_MODE_SETPROPERTY && arg) {
    struct drm_mode_connector_set_property* s = (struct drm_mode_connector_set_property*)arg;
    if (s->prop_id == DRM_PROP_DPMS) g_connector.dpms = (uint32_t)s->value;
    if (tracing())
      fprintf(stderr, "[drm] MODE_SETPROPERTY connector=%u prop=%u value=%llu -> ok\n",
              s->connector_id, s->prop_id, (unsigned long long)s->value);
    return 0;
  }

  // MODE_OBJ_SETPROPERTY (drmModeObjectSetProperty): accept + record.
  if (type == DRM_TYPE && nr == DRM_NR_MODE_OBJ_SETPROPERTY && arg) {
    struct drm_mode_obj_set_property* s = (struct drm_mode_obj_set_property*)arg;
    if (s->prop_id == DRM_PROP_DPMS) g_connector.dpms = (uint32_t)s->value;
    if (tracing())
      fprintf(stderr, "[drm] MODE_OBJ_SETPROPERTY obj=%u type=0x%x prop=%u value=%llu -> ok\n",
              s->obj_id, s->obj_type, s->prop_id, (unsigned long long)s->value);
    return 0;
  }

  // MODE_PAGE_FLIP (legacy flip): drmModePageFlip(fd, crtc, fb, flags, user_data).
  if (type == DRM_TYPE && nr == DRM_NR_MODE_PAGE_FLIP && arg) {
    struct drm_mode_crtc_page_flip* pf = (struct drm_mode_crtc_page_flip*)arg;
    uint32_t crtc = pf->crtc_id ? pf->crtc_id : 1;
    struct crtc_state* cr = crtc_by_id(crtc);
    struct plane_state* pl = plane_by_id(crtc);
    cr->fb_id = pf->fb_id;
    pl->fb_id = pf->fb_id;
    pl->crtc_id = crtc;
    if (pf->flags & DRM_MODE_PAGE_FLIP_EVENT) event_enqueue(pf->user_data, crtc);
    if (pf->fb_id) drm_present_fb(pf->fb_id, crtc);
    if (tracing())
      fprintf(stderr, "[drm] MODE_PAGE_FLIP crtc_id=%u fb_id=0x%x flags=0x%x user_data=0x%llx%s\n",
              pf->crtc_id, pf->fb_id, pf->flags, (unsigned long long)pf->user_data,
              (pf->flags & DRM_MODE_PAGE_FLIP_EVENT) ? " (event queued)" : "");
    return 0;
  }

  // MODE_ATOMIC: atomic commit. Parse the (objs, props, prop_values) arrays and apply
  // each property by id. This is the north-star path: ALLOW_MODESET carries the MODE_ID
  // blob, and a page-flip carries a fresh FB_ID + PAGE_FLIP_EVENT.
  if (type == DRM_TYPE && nr == DRM_NR_MODE_ATOMIC && arg) {
    struct drm_mode_atomic* a = (struct drm_mode_atomic*)arg;
    uint32_t mode_blob = 0;
    int is_modeset = (a->flags & DRM_MODE_ATOMIC_ALLOW_MODESET) != 0;
    int test_only  = (a->flags & DRM_MODE_ATOMIC_TEST_ONLY) != 0;

    // The object ids the commit touches (crtc/plane/connector all live in the id-1/id-2
    // namespace: id 1 = external DP CRTC+plane+connector, id 2 = builtin eDP). We read
    // objs_ptr so each property routes to its OWN object's state instead of the primary.
    uint32_t* objs = (uint32_t*)(uintptr_t)a->objs_ptr;
    uint32_t* cnts = (uint32_t*)(uintptr_t)a->count_props_ptr;
    uint32_t* props = (uint32_t*)(uintptr_t)a->props_ptr;
    uint64_t* vals = (uint64_t*)(uintptr_t)a->prop_values_ptr;

    // Per-plane flips to present after the commit: flip_fb[id-1], resolved to flip_crtc[id-1].
    uint32_t flip_fb[2]   = {0, 0};
    uint32_t flip_crtc[2] = {0, 0};

    uint32_t off = 0;
    for (uint32_t i = 0; i < a->count_objs; i++) {
      uint32_t obj_id = objs ? objs[i] : 0;
      if (obj_id != 1 && obj_id != 2) obj_id = 1;      // never index out of range
      uint32_t n = cnts ? cnts[i] : 0;

      // Classify the object by the properties it carries (the id namespace is shared across
      // CRTC/plane/connector, so the props are the only disambiguator). Only plane vs connector
      // matters for routing CRTC_ID; ACTIVE/MODE_ID and the SRC/CRTC rect props are unambiguous.
      int is_plane = 0, is_conn = 0;
      for (uint32_t j = 0; j < n; j++) {
        uint32_t pid = props ? props[off + j] : 0;
        if (pid == DRM_PROP_FB_ID || pid == DRM_PROP_TYPE ||
            (pid >= DRM_PROP_SRC_X && pid <= DRM_PROP_CRTC_H)) is_plane = 1;
        else if (pid == DRM_PROP_DPMS) is_conn = 1;
      }

      struct crtc_state* cr = crtc_by_id(obj_id);
      struct plane_state* pl = plane_by_id(obj_id);
      struct connector_state* cn = connector_by_id(obj_id);

      for (uint32_t j = 0; j < n; j++) {
        uint32_t pid = props ? props[off + j] : 0;
        uint64_t val = vals ? vals[off + j] : 0;
        switch (pid) {
          case DRM_PROP_ACTIVE:  cr->active = (uint32_t)val; break;
          case DRM_PROP_MODE_ID:
            cr->mode_blob = (uint32_t)val;
            if (val) {
              struct blob_slot* s = blob_find((uint32_t)val);
              if (s && s->len >= sizeof(struct drm_mode_modeinfo)) {
                memcpy(&cr->mode, s->data, sizeof(struct drm_mode_modeinfo));
                cr->mode_valid = 1;
                mode_blob = (uint32_t)val;
              }
            } else { cr->mode_valid = 0; memset(&cr->mode, 0, sizeof cr->mode); }
            break;
          case DRM_PROP_CRTC_ID:
            if (is_conn) cn->crtc_id = (uint32_t)val;
            if (is_plane) pl->crtc_id = (uint32_t)val;
            break;
          case DRM_PROP_FB_ID:
            pl->fb_id = (uint32_t)val;
            cr->fb_id = (uint32_t)val;
            if (obj_id >= 1 && obj_id <= 2) flip_fb[obj_id - 1] = (uint32_t)val;
            break;
          case DRM_PROP_SRC_X:   pl->src_x = val; break;
          case DRM_PROP_SRC_Y:   pl->src_y = val; break;
          case DRM_PROP_SRC_W:   pl->src_w = val; break;
          case DRM_PROP_SRC_H:   pl->src_h = val; break;
          case DRM_PROP_CRTC_X:  pl->crtc_x = val; break;
          case DRM_PROP_CRTC_Y:  pl->crtc_y = val; break;
          case DRM_PROP_CRTC_W:  pl->crtc_w = val; break;
          case DRM_PROP_CRTC_H:  pl->crtc_h = val; break;
          case DRM_PROP_DPMS:    cn->dpms = (uint32_t)val; break;
          default: break;                    // IN_FENCE_FD / OUT_FENCE_PTR / GAMMA etc: ignore
        }
      }
      off += n;
    }

    // Present each plane's newly-committed framebuffer to its CRTC. Plane N scans out to
    // CRTC N (the plane's CRTC_ID property when set), so both outputs present their frame.
    uint32_t event_crtc = 0;
    if (!test_only) {
      for (int pi = 0; pi < 2; pi++) {
        if (!flip_fb[pi]) continue;
        struct plane_state* pl = plane_by_id(pi + 1);
        uint32_t crtc = pl->crtc_id ? pl->crtc_id : (uint32_t)(pi + 1);
        flip_crtc[pi] = crtc;
        if (!event_crtc) event_crtc = crtc;
        drm_present_fb(flip_fb[pi], crtc);
      }
    }
    if (!test_only && (a->flags & DRM_MODE_PAGE_FLIP_EVENT))
      event_enqueue(a->user_data, event_crtc);
    if (tracing())
      fprintf(stderr, "[drm] MODE_ATOMIC flags=0x%x objs=%u%s%s%s fb_id=0x%x/0x%x mode_blob=%u "
              "crtc=%u/%u -> ok\n",
              a->flags, a->count_objs, is_modeset ? " MODESET" : "", test_only ? " TEST" : "",
              (a->flags & DRM_MODE_PAGE_FLIP_EVENT) ? " FLIP" : "", flip_fb[0], flip_fb[1],
              mode_blob, flip_crtc[0], flip_crtc[1]);
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
