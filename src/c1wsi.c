// c1wsi.c — THE MISSING WSI: VK_KHR_surface + VK_KHR_wayland_surface + VK_KHR_swapchain, provided
// by the guest-side ICD so that Mesa/zink can present a WINDOW instead of only rendering offscreen.
//
// WHY THIS FILE EXISTS, and why it is the only thing that was missing.
// B7 got the whole render chain up headless (app -> zink -> Vulkan -> MoltenVK -> Metal) and named
// WSI as blocker C. Two measurements pin it exactly:
//
//   1. `GALLIUM_DRIVER=zink` under the `swrast` DRI driver RENDERS in a real Wayland client and
//      presents NOTHING: `libEGL debug: did not find extension DRI_Kopper version 1`, and the
//      compositor logs no create_pool / create_buffer / attach for the whole run, while the client's
//      eglSwapBuffers keeps returning EGL_TRUE. (llvmpipe on the same client: 8 frames, per-pixel
//      verified.) The shm present path needs a front-buffer flush that this loader/driver pair does
//      not wire up.
//   2. `MESA_LOADER_DRIVER_OVERRIDE=zink` loads the real zink+kopper driver, which is a COMPLETE WSI
//      client — and it dies on its first frame with `fault_addr=0 pc=0`. Disassembling the call site
//      (libgallium_dri.so + 0x10a4f84):
//           cmp w1, #0x3b9add88   ; sType == 1000005000 ? (XCB_SURFACE_CREATE_INFO_KHR)
//           b.eq <xcb path>
//           ldr x4, [x19,#0x2bd8] ; else the WAYLAND slot
//           blr x4                ; NULL
//      zink calls vkCreateWaylandSurfaceKHR through a table slot it never NULL-checks. MoltenVK has
//      VK_EXT_metal_surface and no wayland surface, so the slot is empty.
//
// So the shape of the fix is: give zink the Vulkan WSI it is already written against. Everything
// else — swapchain management, acquire/present pacing, damage — is kopper's, and it works.
//
// WHY READBACK RATHER THAN VK_EXT_metal_surface, stated with the numbers rather than asserted.
// MoltenVK does have VK_EXT_metal_surface, and pointing it at a CAMetalLayer would avoid the copy
// entirely. It is the wrong v1 here for three reasons, in order of weight:
//   * It bypasses the compositor. src/wlsrv.c composites MANY surfaces — toplevel plus popups, with
//     stacking, damage, hit-testing and frame callbacks — into ONE framebuffer that window.m
//     presents. A Metal sublayer per wl_surface would take a GTK menu out of that pipeline, and the
//     menu is exactly where this project's compositing bugs live.
//   * It cannot work headless. ./shim_wlh has no Cocoa and no Metal, and it is the binary every GUI
//     measurement uses precisely because it needs no window-server lock. A metal_surface path would
//     make the GPU route untestable except while holding the one screen.
//   * The copy is affordable, measured, not assumed. The shim's present path is 0.117 ms + 0.27 ns/px
//     (B7/B5: 0.67 ms at 1080p) against ~85 ms/frame of llvmpipe. This file adds a GPU->host copy
//     plus one memcpy into the shm buffer on top of that; C1WSI_STATS=1 prints the measured cost per
//     present so the claim stays measured. If it ever dominates, the fix is to import the wl_shm
//     pages as VkDeviceMemory (VK_EXT_external_memory_host) and delete the memcpy — not to abandon
//     the compositor.
//
// THE ONE QUEUE. C2 measured queueCount=1 on MoltenVK's graphics family: there is exactly one
// VkQueue on this stack, so zink's draw submits and this file's readback submits are the SAME
// externally-synchronised queue. Two consequences are designed for rather than discovered:
//   * present submits on the queue the caller handed us (pPresentInfo's queue), never a private one;
//   * acquire needs to signal a BINARY semaphore, for which Vulkan offers no host-side signal at
//     all, so it must submit — it uses the queue cached from vkGetDeviceQueue, which is that same
//     single queue. Callers must not call vkAcquireNextImageKHR concurrently with a submit on it;
//     that is the ordinary Vulkan external-synchronisation rule for a queue, not an extra one.
//
// SCOPE, stated so nobody over-reads it: one wl_surface per swapchain, FIFO/IMMEDIATE present modes,
// BGRA8 only, no VK_KHR_display, no incremental damage (the whole surface is damaged each present).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <errno.h>

#include <vulkan/vulkan_core.h>

// Wayland, declared rather than included: this ICD must not acquire a DT_NEEDED on
// libwayland-client. Every symbol below is resolved with dlsym out of the copy the CLIENT already
// has loaded — if the client is not a Wayland client, none of this is ever reached.
struct wl_display; struct wl_surface; struct wl_proxy; struct wl_event_queue; struct wl_interface;

typedef struct VkWaylandSurfaceCreateInfoKHR_ {
  VkStructureType sType; const void* pNext; VkFlags flags;
  struct wl_display* display; struct wl_surface* surface;
} C1WaylandSurfaceCreateInfo;
#define C1_STYPE_WAYLAND_SURFACE_CREATE_INFO 1000006000

#ifndef VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME
#define VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME "VK_KHR_wayland_surface"
#define VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME "VK_EXT_headless_surface"
#endif

static int g_v;                                  // C1WSI_VERBOSE
static int g_stats;                              // C1WSI_STATS
// P3 knobs. Both default to the NEW behaviour and are turned OFF one at a time, so an A/B moves
// exactly one variable inside ONE binary — the same sha in both arms of every number below.
static int g_noimport;                           // C1WSI_NOIMPORT=1 -> staging buffer + memcpy
static int g_nodefer;                            // C1WSI_NODEFER=1  -> block on the fence in present
static int g_ext_mem_host;                       // VK_EXT_external_memory_host got enabled
#define LOG(...) do { if (g_v) { fprintf(stderr, "[c1wsi] " __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define WARN(...) do { fprintf(stderr, "[c1wsi] " __VA_ARGS__); fputc('\n', stderr); } while (0)

static uint64_t now_ns(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// ============================================================ C1WSI_KOPPER: reach the WSI at all
//
// WITHOUT THIS, NOTHING IN THIS FILE IS EVER CALLED. That is not a figure of speech: a 353-frame
// glmark2 run produced zero vkCreateWaylandSurfaceKHR, zero vkCreateSwapchainKHR and zero
// vkQueuePresentKHR, and rendered 100% black. The cause is one env var that Mesa 25.0.7 reads
// twice, for two different questions, and answers with the same bit.
//
//   src/egl/main/eglapi.c:687     disp->Options.ForceSoftware = LIBGL_ALWAYS_SOFTWARE
//   src/gallium/drivers/zink/zink_screen.c:1680
//                                 bool cpu               = LIBGL_ALWAYS_SOFTWARE
//
// In platform_wayland.c's `dri2_initialize_wayland_swrast` -- which is the function that runs for
// zink, because `dri2_initialize_wayland` routes Options.Zink there -- the kopper machinery is
// selected by Options.Zink alone (lines 3031, 3077, 3103: the kopper registry listener, the
// kopper loader extensions, and dri2_wl_kopper_display_vtbl). kopper is what calls
// kopperSetSurfaceCreateInfo, which fills in a VkWaylandSurfaceCreateInfoKHR, which is the ONLY
// thing that ever reaches this file. But eleven lines earlier, at 3046:
//
//     if (!dri2_initialize_wayland_drm_extensions(dri2_dpy) && !disp->Options.ForceSoftware)
//        goto cleanup;
//
// and dri2_initialize_wayland_drm_extensions cannot succeed here: it returns true only once
// fd_render_gpu names an open DRM node, obtained either from zwp_linux_dmabuf_v1 feedback or from
// wl_drm authentication. There is no /dev/dri in the rootfs and wlsrv.c advertises neither
// protocol, so it returns false and, with ForceSoftware clear, EGL init FAILS.
//
// eglInitialize then retries (eglapi.c:709-719): Options.Zink is already true so the FallbackZink
// arm is skipped, and it lands on `Options.Zink = FALSE; ForceSoftware = TRUE`. That third attempt
// succeeds -- as **swrast**. driver_name is "swrast", but MESA_LOADER_DRIVER_OVERRIDE=zink still
// makes dri2_load_driver open zink_dri.so, so zink is driving a real GPU while wearing the swrast
// loader (putImage into a wl_shm pool) and dri2_wl_swrast_display_vtbl, and egl_dri2.c:593 leaves
// dri2_dpy->kopper NULL. That is the black-frame state exactly, and it is why the FPS looked fine:
// the pipeline runs, and its result is handed to a readback path zink does not implement.
//
// Setting LIBGL_ALWAYS_SOFTWARE=1 clears the 3046 wall -- and zink then refuses to start at all,
// because `cpu` makes zink_get_cpu_device_type demand a VK_PHYSICAL_DEVICE_TYPE_CPU physical
// device and our ICD honestly reports the M2 Max:
//     MESA: error: ZINK: CPU device requested but none found!
//
// So the two readers need OPPOSITE answers, and the semantics we actually want -- "there is no DRM
// device, but the Vulkan device is a real GPU" -- is not expressible in Mesa 25.0.7. It is a Mesa
// bug: zink+kopper on a compositor that advertises no dmabuf is unreachable on hardware.
//
// THE ORDERING IS WHAT MAKES THIS FIXABLE FROM HERE, and it is guaranteed rather than lucky:
// eglInitialize reads the var (687) BEFORE it calls _eglDriver.Initialize, and zink reads it inside
// zink_create_screen, which is reached from dri2_create_screen much later -- after zink has already
// called vkCreateInstance, which is what makes the Vulkan loader dlopen THIS library. So a
// constructor here runs strictly between the two reads: EGL has already banked ForceSoftware=true,
// and zink has not yet asked. Removing the variable now answers both questions correctly.
//
// Deliberately OPT-IN, and it must stay opt-in: it is the one variable in an A/B that shows this
// file being reached at all, so `C1WSI_KOPPER=1` vs unset is the negative control for every
// non-black frame claimed on this path. Both arms are the same binary and the same sha.
__attribute__((constructor)) static void c1wsi_kopper_ungate(void) {
  const char* want = getenv("C1WSI_KOPPER");
  if (!want || want[0] != '1') return;
  const char* sw = getenv("LIBGL_ALWAYS_SOFTWARE");
  if (!sw) {
    fprintf(stderr, "[c1wsi] C1WSI_KOPPER=1 but LIBGL_ALWAYS_SOFTWARE is unset -- EGL will have "
                    "taken the non-kopper path already; this is a no-op\n");
    return;
  }
  unsetenv("LIBGL_ALWAYS_SOFTWARE");
  unsetenv("D3D_ALWAYS_SOFTWARE");
  fprintf(stderr, "[c1wsi] C1WSI_KOPPER: dropped LIBGL_ALWAYS_SOFTWARE=%s after EGL read it, "
                  "before zink's choose_pdev does\n", sw);
}

// ============================================================================ host Vulkan procs
// Provided by b7icd.c: MoltenVK's own vk_icdGetInstanceProcAddr, and the device proc addr it hands
// out. These are HOST function pointers called directly (B7's measured result: 11.2 ns/call).
typedef void* (*PFN_gipa_t)(void* instance, const char* name);
static PFN_gipa_t   g_gipa;
static VkInstance   g_instance;

static struct {
  PFN_vkGetDeviceProcAddr                     gdpa;
  PFN_vkGetPhysicalDeviceMemoryProperties     memprops;
  PFN_vkCreateImage                           CreateImage;
  PFN_vkDestroyImage                          DestroyImage;
  PFN_vkGetImageMemoryRequirements            ImageMemReq;
  PFN_vkAllocateMemory                        AllocMem;
  PFN_vkFreeMemory                            FreeMem;
  PFN_vkBindImageMemory                       BindImage;
  PFN_vkCreateBuffer                          CreateBuffer;
  PFN_vkDestroyBuffer                         DestroyBuffer;
  PFN_vkGetBufferMemoryRequirements           BufferMemReq;
  PFN_vkBindBufferMemory                      BindBuffer;
  PFN_vkMapMemory                             MapMemory;
  PFN_vkCreateCommandPool                     CreateCmdPool;
  PFN_vkDestroyCommandPool                    DestroyCmdPool;
  PFN_vkAllocateCommandBuffers                AllocCmdBufs;
  PFN_vkBeginCommandBuffer                    BeginCmd;
  PFN_vkEndCommandBuffer                      EndCmd;
  PFN_vkResetCommandBuffer                    ResetCmd;
  PFN_vkCmdPipelineBarrier                    CmdBarrier;
  PFN_vkCmdCopyImageToBuffer                  CmdCopyImageToBuffer;
  PFN_vkCreateFence                           CreateFence;
  PFN_vkDestroyFence                          DestroyFence;
  PFN_vkResetFences                           ResetFences;
  PFN_vkWaitForFences                         WaitFences;
  PFN_vkQueueSubmit                           QueueSubmit;
  PFN_vkDeviceWaitIdle                        DeviceWaitIdle;
  // P3: VK_EXT_external_memory_host. Present only if the device has the extension AND
  // c1_CreateDevice managed to enable it; every use is guarded on the pointer being non-NULL.
  PFN_vkGetMemoryHostPointerPropertiesEXT     HostPtrProps;
} vk;

#define DGET(dev, field, name) \
  do { if (!vk.field) vk.field = (PFN_vk##name)vk.gdpa(dev, "vk" #name); } while (0)

static int load_device_procs(VkDevice dev) {
  if (!vk.gdpa) return 0;
  DGET(dev, CreateImage, CreateImage);
  DGET(dev, DestroyImage, DestroyImage);
  DGET(dev, ImageMemReq, GetImageMemoryRequirements);
  DGET(dev, AllocMem, AllocateMemory);
  DGET(dev, FreeMem, FreeMemory);
  DGET(dev, BindImage, BindImageMemory);
  DGET(dev, CreateBuffer, CreateBuffer);
  DGET(dev, DestroyBuffer, DestroyBuffer);
  DGET(dev, BufferMemReq, GetBufferMemoryRequirements);
  DGET(dev, BindBuffer, BindBufferMemory);
  DGET(dev, MapMemory, MapMemory);
  DGET(dev, CreateCmdPool, CreateCommandPool);
  DGET(dev, DestroyCmdPool, DestroyCommandPool);
  DGET(dev, AllocCmdBufs, AllocateCommandBuffers);
  DGET(dev, BeginCmd, BeginCommandBuffer);
  DGET(dev, EndCmd, EndCommandBuffer);
  DGET(dev, ResetCmd, ResetCommandBuffer);
  DGET(dev, CmdBarrier, CmdPipelineBarrier);
  DGET(dev, CmdCopyImageToBuffer, CmdCopyImageToBuffer);
  DGET(dev, CreateFence, CreateFence);
  DGET(dev, DestroyFence, DestroyFence);
  DGET(dev, ResetFences, ResetFences);
  DGET(dev, WaitFences, WaitForFences);
  DGET(dev, QueueSubmit, QueueSubmit);
  DGET(dev, DeviceWaitIdle, DeviceWaitIdle);
  if (!vk.HostPtrProps && g_ext_mem_host)
    vk.HostPtrProps = (PFN_vkGetMemoryHostPointerPropertiesEXT)
                      vk.gdpa(dev, "vkGetMemoryHostPointerPropertiesEXT");
  if (!vk.memprops && g_gipa && g_instance)
    vk.memprops = (PFN_vkGetPhysicalDeviceMemoryProperties)
                  g_gipa(g_instance, "vkGetPhysicalDeviceMemoryProperties");
  return vk.CreateImage && vk.AllocMem && vk.QueueSubmit && vk.CmdCopyImageToBuffer && vk.memprops;
}

// ============================================================================ wayland, by dlsym
static struct {
  void* lib;
  struct wl_event_queue* (*display_create_queue)(struct wl_display*);
  void  (*event_queue_destroy)(struct wl_event_queue*);
  int   (*display_roundtrip_queue)(struct wl_display*, struct wl_event_queue*);
  int   (*display_dispatch_queue_pending)(struct wl_display*, struct wl_event_queue*);
  int   (*display_flush)(struct wl_display*);
  void* (*proxy_create_wrapper)(void*);
  void  (*proxy_wrapper_destroy)(void*);
  void  (*proxy_set_queue)(struct wl_proxy*, struct wl_event_queue*);
  struct wl_proxy* (*proxy_marshal_flags)(struct wl_proxy*, uint32_t opcode,
                                          const struct wl_interface*, uint32_t version,
                                          uint32_t flags, ...);
  int   (*proxy_add_listener)(struct wl_proxy*, void (**)(void), void*);
  uint32_t (*proxy_get_version)(struct wl_proxy*);
  void  (*proxy_destroy)(struct wl_proxy*);
  const struct wl_interface* registry_iface;
  const struct wl_interface* shm_iface;
  const struct wl_interface* shm_pool_iface;
  const struct wl_interface* buffer_iface;
} wl;

#define WSYM(f, n) do { *(void**)&wl.f = dlsym(wl.lib, n); if (!wl.f) { WARN("missing %s in libwayland-client", n); return 0; } } while (0)

static int wl_up(void) {
  if (wl.lib) return 1;
  // The client has it loaded already; dlopen by soname just bumps the refcount and gives a handle.
  wl.lib = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
  if (!wl.lib) { WARN("dlopen(libwayland-client.so.0) failed: %s", dlerror()); return 0; }
  WSYM(display_create_queue, "wl_display_create_queue");
  WSYM(event_queue_destroy, "wl_event_queue_destroy");
  WSYM(display_roundtrip_queue, "wl_display_roundtrip_queue");
  WSYM(display_dispatch_queue_pending, "wl_display_dispatch_queue_pending");
  WSYM(display_flush, "wl_display_flush");
  WSYM(proxy_create_wrapper, "wl_proxy_create_wrapper");
  WSYM(proxy_wrapper_destroy, "wl_proxy_wrapper_destroy");
  WSYM(proxy_set_queue, "wl_proxy_set_queue");
  WSYM(proxy_marshal_flags, "wl_proxy_marshal_flags");
  WSYM(proxy_add_listener, "wl_proxy_add_listener");
  WSYM(proxy_get_version, "wl_proxy_get_version");
  WSYM(proxy_destroy, "wl_proxy_destroy");
  wl.registry_iface = (const struct wl_interface*)dlsym(wl.lib, "wl_registry_interface");
  wl.shm_iface      = (const struct wl_interface*)dlsym(wl.lib, "wl_shm_interface");
  wl.shm_pool_iface = (const struct wl_interface*)dlsym(wl.lib, "wl_shm_pool_interface");
  wl.buffer_iface   = (const struct wl_interface*)dlsym(wl.lib, "wl_buffer_interface");
  if (!wl.registry_iface || !wl.shm_iface || !wl.shm_pool_iface || !wl.buffer_iface) {
    WARN("libwayland-client is missing an interface symbol"); return 0; }
  return 1;
}

// Wire opcodes, from the core protocol. Written out because there is no wayland-scanner on this
// host and the generated headers are not in sdk/.
enum { WL_DISPLAY_GET_REGISTRY = 1 };
enum { WL_REGISTRY_BIND = 0 };
enum { WL_SHM_CREATE_POOL = 0 };
enum { WL_SHM_POOL_CREATE_BUFFER = 0, WL_SHM_POOL_DESTROY = 1 };
enum { WL_BUFFER_DESTROY = 0 };
enum { WL_SURFACE_ATTACH = 1, WL_SURFACE_DAMAGE = 2, WL_SURFACE_COMMIT = 6 };
enum { WL_SHM_FORMAT_ARGB8888 = 0, WL_SHM_FORMAT_XRGB8888 = 1 };

// ============================================================================ objects
#define C1_SURFACE_MAGIC 0xC1105EA1C1105EA1ULL
struct c1_surface {
  uint64_t magic;
  struct wl_display* dpy;
  struct wl_surface* surf;
  int headless;                 // VK_EXT_headless_surface: render and time, present nowhere
};

#define MAX_IMG 4
struct c1_swapchain {
  uint64_t magic;
  VkDevice dev;
  struct c1_surface* s;
  uint32_t w, h, n, next;
  VkFormat fmt;
  VkImage img[MAX_IMG];
  VkDeviceMemory imem[MAX_IMG];
  VkCommandBuffer cb[MAX_IMG];
  VkFence fence[MAX_IMG];
  VkBuffer stage;
  VkDeviceMemory smem;
  uint8_t* smap;
  VkCommandPool pool;
  // wayland side
  struct wl_event_queue* q;
  struct wl_proxy* shm;
  struct wl_proxy* wlpool;
  struct wl_proxy* wbuf[MAX_IMG];
  uint8_t* shmmap;                               // ALIGNED base of the n slots (import point)
  size_t shmsize;                                // bytes from shmmap that are imported
  uint8_t* shmraw;                               // what mmap actually returned
  size_t shmraw_size;
  size_t shmoff;                                 // shmmap - shmraw, also the pool file offset
  int released[MAX_IMG];
  // P3. `imported` means sc->stage is a VkBuffer bound to the wl_shm pool's OWN pages
  // (VK_EXT_external_memory_host), so vkCmdCopyImageToBuffer writes the frame straight into the
  // memory the compositor reads and there is no memcpy at all. When it is 0 the buffer is a
  // driver-allocated host-visible staging area and the memcpy is back.
  //
  // Either way the buffer now covers the WHOLE pool (n slots), not one frame. That is what makes
  // deferral possible: with a single one-frame staging buffer, frame N+1's copy would overwrite
  // frame N's bytes before frame N had been posted.
  int imported;
  // P3 ZERO-COPY. `zc` means the SWAPCHAIN IMAGES THEMSELVES live in the wl_shm pool: LINEAR
  // tiling, bound directly to the imported pool memory at slot offsets, so zink renders straight
  // into the pages the compositor reads and present issues NO blit at all. `stride` and `slot` are
  // the pool geometry, which zero-copy takes from the image's own rowPitch rather than assuming.
  int zc;
  size_t stride, slot;
  int pending;                                   // image submitted but not yet waited-on/attached
  uint64_t ns_submit;
  // THE FRAME PERIOD, measured from inside, present-entry to present-entry, and only after the
  // first WARMUP presents. Wall-clock-over-frame-count cannot do this job: ~1.3 s of every run of
  // this client is process start, Mesa init and Metal shader compilation, and differencing two run
  // lengths to cancel it turned a 164 ms startup wobble into 0.55 ms/frame of phantom slope — big
  // enough to invert the sign of the result being measured. This number has no startup in it.
  uint64_t last_present_ns, ns_frame, frames_timed;
  // THE DISTRIBUTION, not just the mean. P1 and I got 1.29x and 1.07x for the same patch, and one
  // live explanation was that a mean and a min disagree legitimately when a change removes a slow
  // TAIL rather than shifting the centre. A mean cannot answer that about itself; only the
  // percentiles can, so they are recorded rather than argued about. 8192 samples covers every run
  // this harness does; beyond that it keeps the FIRST 8192, which is stated because silently
  // keeping the last N would bias toward the warmest part of the run.
  uint32_t nsamp;
  uint32_t samp_us10[8192];                      // frame interval in tenths of a microsecond
  // THE WAIT IS TIMED SEPARATELY FROM THE COPY, and that is not pedantry: the first version billed
  // both to "memcpy" and reported 25 ms/frame for a 4 MiB copy, which is ~100x what a memcpy costs
  // and would have sent the next person to optimise the wrong thing. Almost all of it was the
  // compositor not having released the buffer yet -- i.e. back-pressure, which is correct
  // behaviour and not a cost of this design at all.
  uint64_t presents, ns_gpu, ns_wait, ns_copy;
};
#define C1_SWAPCHAIN_MAGIC 0xC11053CA1E5CULL

// The single queue (C2's measurement), cached from vkGetDeviceQueue. Used ONLY to signal the
// binary semaphore/fence that vkAcquireNextImageKHR must signal; present uses the caller's queue.
static VkQueue g_queue;
static VkPhysicalDevice g_phys;
static size_t g_import_align;                    // minImportedHostPointerAlignment, 0 = no import

// `avoid` exists because of a rule MoltenVK states in its own words and B7's board post did not
// have: "Host-coherent VkDeviceMemory objects cannot be associated with optimal-tiling images."
// On Apple silicon the memory is unified, so a type can be DEVICE_LOCAL *and* HOST_COHERENT --
// and picking that type for an OPTIMAL-tiling image fails with VK_ERROR_OUT_OF_DEVICE_MEMORY,
// which reads as "out of memory" on a device reporting a 32 GiB heap. Metal's private/shared
// storage modes are the underlying constraint, so this is a real rule and not a MoltenVK bug.
static uint32_t pick_memtype(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) {
  VkPhysicalDeviceMemoryProperties mp;
  vk.memprops(g_phys, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
    VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
    if ((bits & (1u << i)) && (f & want) == want && !(f & avoid)) return i;
  }
  return UINT32_MAX;
}

// ============================================================================ wl_shm plumbing
static struct wl_proxy* g_found_shm_registry;    // set by the registry listener below
static uint32_t g_found_shm_name, g_found_shm_ver;

static void reg_global(void* data, struct wl_proxy* reg, uint32_t name,
                       const char* iface, uint32_t ver) {
  (void)data; (void)reg;
  if (iface && !strcmp(iface, "wl_shm")) { g_found_shm_name = name; g_found_shm_ver = ver; }
}
static void reg_global_remove(void* data, struct wl_proxy* reg, uint32_t name) {
  (void)data; (void)reg; (void)name;
}
static void (*reg_listener[])(void) = { (void(*)(void))reg_global, (void(*)(void))reg_global_remove };

static void buf_release(void* data, struct wl_proxy* b) {
  (void)b;
  int* flag = (int*)data;
  if (flag) *flag = 1;
}
static void (*buf_listener[])(void) = { (void(*)(void))buf_release };

// An anonymous file for the pool. memfd_create is what Mesa uses and what src/syscalls_ext.c
// implements (as mkstemp+unlink), so this is the path the compositor is already proven against.
static int anon_file(size_t size) {
  int fd = (int)syscall(279 /* SYS_memfd_create on aarch64 */, "c1wsi", 0u);
  if (fd < 0) { WARN("memfd_create failed (errno %d)", errno); return -1; }
  if (ftruncate(fd, (off_t)size) < 0) { close(fd); WARN("ftruncate failed"); return -1; }
  return fd;
}

// The Wayland half of the setup below: bind wl_shm on our own queue. A HEADLESS surface skips it
// entirely -- there is no compositor to bind to -- and every step after it (pool, images, staging
// copy, GPU submit) is identical, so the two paths differ only in what becomes of the finished
// pixels. That is what makes a headless run a fair measurement of the same work.
static int wl_bind_shm(struct c1_swapchain* sc) {
  if (!wl_up()) return 0;
  struct wl_display* dpy = sc->s->dpy;
  sc->q = wl.display_create_queue(dpy);
  if (!sc->q) { WARN("wl_display_create_queue failed"); return 0; }

  // A proxy wrapper puts the registry (and everything created from it) on OUR queue, so dispatching
  // it here cannot steal events the application's own thread is waiting for. This is exactly what
  // Mesa's Wayland WSI does, for the same reason.
  void* w = wl.proxy_create_wrapper(dpy);
  if (!w) { WARN("wl_proxy_create_wrapper failed"); return 0; }
  wl.proxy_set_queue((struct wl_proxy*)w, sc->q);
  struct wl_proxy* reg = wl.proxy_marshal_flags((struct wl_proxy*)w, WL_DISPLAY_GET_REGISTRY,
                                                wl.registry_iface,
                                                wl.proxy_get_version((struct wl_proxy*)w), 0, NULL);
  wl.proxy_wrapper_destroy(w);
  if (!reg) { WARN("get_registry failed"); return 0; }
  g_found_shm_name = 0;
  wl.proxy_add_listener(reg, reg_listener, NULL);
  wl.display_roundtrip_queue(dpy, sc->q);
  if (!g_found_shm_name) { WARN("the compositor advertises no wl_shm — cannot present"); return 0; }

  uint32_t ver = g_found_shm_ver < 1 ? 1 : 1;      // v1 is all this needs
  sc->shm = wl.proxy_marshal_flags(reg, WL_REGISTRY_BIND, wl.shm_iface, ver, 0,
                                   g_found_shm_name, "wl_shm", ver, NULL);
  wl.proxy_destroy(reg);
  if (!sc->shm) { WARN("bind wl_shm failed"); return 0; }

  return 1;
}

static int wl_setup(struct c1_swapchain* sc) {
  if (!sc->s->headless && !wl_bind_shm(sc)) return 0;

  size_t stride = sc->stride;
  // THE POOL IS OVER-ALLOCATED AND THE SLOTS START AT AN ALIGNED OFFSET INSIDE IT.
  // VK_EXT_external_memory_host will only import a pointer that is a multiple of
  // minImportedHostPointerAlignment (MEASURED on MoltenVK 1.4 / M2 Max: 16384) with a size that is
  // also a multiple of it. The guest's mmap does not hand back 16 KiB-aligned addresses, and the
  // first attempt at this silently fell back to the staging path for exactly that reason — the
  // verbose log said "import unavailable" and the numbers looked like a smaller win than they are.
  //
  // Padding the FILE and picking an aligned point INSIDE the mapping avoids MAP_FIXED entirely:
  // wl_shm buffer offsets are file offsets, so shifting every slot by the same `shmoff` keeps the
  // client and the compositor looking at the same bytes.
  size_t align = g_import_align > 1 ? g_import_align : 1;
  size_t slots = sc->slot * sc->n;
  sc->shmsize = (slots + align - 1) / align * align;
  sc->shmraw_size = sc->shmsize + align;
  int fd = anon_file(sc->shmraw_size);
  if (fd < 0) return 0;
  sc->shmraw = mmap(NULL, sc->shmraw_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (sc->shmraw == MAP_FAILED) { sc->shmraw = NULL; close(fd); WARN("mmap pool failed"); return 0; }
  sc->shmoff = (size_t)((align - ((uintptr_t)sc->shmraw % align)) % align);
  sc->shmmap = sc->shmraw + sc->shmoff;
  if (sc->s->headless) {
    close(fd);                                      // no compositor wants a dup of it
    for (uint32_t i = 0; i < sc->n; i++) sc->released[i] = 1;
    LOG("headless pool up: %ux%u x%u slots, %zu bytes at +%zu (no compositor)",
        sc->w, sc->h, sc->n, sc->shmsize, sc->shmoff);
    return 1;
  }
  sc->wlpool = wl.proxy_marshal_flags(sc->shm, WL_SHM_CREATE_POOL, wl.shm_pool_iface, 1, 0,
                                      NULL, fd, (int32_t)sc->shmraw_size);
  close(fd);                                        // the compositor holds its own dup
  if (!sc->wlpool) { WARN("create_pool failed"); return 0; }
  for (uint32_t i = 0; i < sc->n; i++) {
    sc->wbuf[i] = wl.proxy_marshal_flags(sc->wlpool, WL_SHM_POOL_CREATE_BUFFER, wl.buffer_iface, 1, 0,
                                         NULL, (int32_t)(sc->shmoff + sc->slot * i),
                                         (int32_t)sc->w,
                                         (int32_t)sc->h, (int32_t)stride,
                                         (uint32_t)WL_SHM_FORMAT_XRGB8888);
    if (!sc->wbuf[i]) { WARN("create_buffer %u failed", i); return 0; }
    wl.proxy_set_queue(sc->wbuf[i], sc->q);
    sc->released[i] = 1;
    wl.proxy_add_listener(sc->wbuf[i], buf_listener, &sc->released[i]);
  }
  LOG("wl_shm pool up: %ux%u x%u buffers, %zu bytes at +%zu (raw %p, align %zu)",
      sc->w, sc->h, sc->n, sc->shmsize, sc->shmoff, (void*)sc->shmraw, align);
  return 1;
}

// ============================================================================ WSI entry points
static VKAPI_ATTR VkResult VKAPI_CALL
c1_CreateWaylandSurfaceKHR(VkInstance inst, const C1WaylandSurfaceCreateInfo* ci,
                           const VkAllocationCallbacks* alloc, VkSurfaceKHR* out) {
  (void)inst; (void)alloc;
  if (!ci || !out) return VK_ERROR_INITIALIZATION_FAILED;
  struct c1_surface* s = calloc(1, sizeof *s);
  if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
  s->magic = C1_SURFACE_MAGIC; s->dpy = ci->display; s->surf = ci->surface;
  *out = (VkSurfaceKHR)(uintptr_t)s;
  LOG("CreateWaylandSurfaceKHR display=%p surface=%p -> %p", (void*)ci->display,
      (void*)ci->surface, (void*)s);
  return VK_SUCCESS;
}

// VK_EXT_headless_surface. WHY IT EARNS ITS PLACE: on the Wayland path every frame ends at the
// compositor's Metal drawable, which is paced to the display. MEASURED with vkmark 2025.01 at
// 800x600, all thirteen scenes -- from `clear` to the blur-heavy `desktop` -- report the SAME
// 8.7 ms. A GPU-bound run cannot do that; it is vsync, and the score is the panel's refresh rate
// rather than anything about the GPU. A headless surface runs the identical pipeline and simply
// has nowhere to put the finished frame, so what is left to measure is the rendering.
typedef struct { VkStructureType sType; const void* pNext; uint32_t flags; } C1HeadlessSurfaceCreateInfo;

static VKAPI_ATTR VkResult VKAPI_CALL
c1_CreateHeadlessSurfaceEXT(VkInstance inst, const C1HeadlessSurfaceCreateInfo* ci,
                            const VkAllocationCallbacks* alloc, VkSurfaceKHR* out) {
  (void)inst; (void)ci; (void)alloc;
  if (!out) return VK_ERROR_INITIALIZATION_FAILED;
  struct c1_surface* s = calloc(1, sizeof *s);
  if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
  s->magic = C1_SURFACE_MAGIC; s->headless = 1;
  *out = (VkSurfaceKHR)(uintptr_t)s;
  LOG("CreateHeadlessSurfaceEXT -> %p (no compositor, no vsync)", (void*)s);
  return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
c1_DestroySurfaceKHR(VkInstance inst, VkSurfaceKHR surf, const VkAllocationCallbacks* a) {
  (void)inst; (void)a;
  struct c1_surface* s = (struct c1_surface*)(uintptr_t)surf;
  if (s && s->magic == C1_SURFACE_MAGIC) { s->magic = 0; free(s); }
}

static VKAPI_ATTR VkResult VKAPI_CALL
c1_GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice pd, uint32_t qf, VkSurfaceKHR s,
                                      VkBool32* sup) {
  (void)pd; (void)qf; (void)s;
  if (sup) *sup = VK_TRUE;
  return VK_SUCCESS;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL
c1_GetPhysicalDeviceWaylandPresentationSupportKHR(VkPhysicalDevice pd, uint32_t qf,
                                                  struct wl_display* d) {
  (void)pd; (void)qf; (void)d; return VK_TRUE;
}

static void fill_caps(VkSurfaceCapabilitiesKHR* c) {
  memset(c, 0, sizeof *c);
  c->minImageCount = 2;
  c->maxImageCount = MAX_IMG;
  // 0xFFFFFFFF means "the surface size is whatever the swapchain asks for", which is what every
  // Wayland WSI reports (a wl_surface has no size of its own until a buffer is attached).
  c->currentExtent.width = 0xFFFFFFFFu;
  c->currentExtent.height = 0xFFFFFFFFu;
  c->minImageExtent.width = 1; c->minImageExtent.height = 1;
  c->maxImageExtent.width = 16384; c->maxImageExtent.height = 16384;
  c->maxImageArrayLayers = 1;
  c->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
  c->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
  c->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR |
                               VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
  c->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                           VK_IMAGE_USAGE_STORAGE_BIT;
}

static VKAPI_ATTR VkResult VKAPI_CALL
c1_GetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice pd, VkSurfaceKHR s,
                                           VkSurfaceCapabilitiesKHR* c) {
  (void)pd; (void)s;
  if (!c) return VK_ERROR_INITIALIZATION_FAILED;
  fill_caps(c);
  return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
c1_GetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice pd, const void* info, void* caps2) {
  (void)pd; (void)info;
  if (!caps2) return VK_ERROR_INITIALIZATION_FAILED;
  VkSurfaceCapabilities2KHR* c2 = (VkSurfaceCapabilities2KHR*)caps2;
  fill_caps(&c2->surfaceCapabilities);
  return VK_SUCCESS;
}

static const VkSurfaceFormatKHR kFormats[] = {
  { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
  { VK_FORMAT_B8G8R8A8_SRGB,  VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
};
#define NFORMATS ((uint32_t)(sizeof kFormats / sizeof kFormats[0]))

static VKAPI_ATTR VkResult VKAPI_CALL
c1_GetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice pd, VkSurfaceKHR s,
                                      uint32_t* n, VkSurfaceFormatKHR* out) {
  (void)pd; (void)s;
  if (!n) return VK_ERROR_INITIALIZATION_FAILED;
  if (!out) { *n = NFORMATS; return VK_SUCCESS; }
  uint32_t c = *n < NFORMATS ? *n : NFORMATS;
  memcpy(out, kFormats, c * sizeof kFormats[0]);
  *n = c;
  return c < NFORMATS ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
c1_GetPhysicalDeviceSurfaceFormats2KHR(VkPhysicalDevice pd, const void* info,
                                       uint32_t* n, VkSurfaceFormat2KHR* out) {
  (void)pd; (void)info;
  if (!n) return VK_ERROR_INITIALIZATION_FAILED;
  if (!out) { *n = NFORMATS; return VK_SUCCESS; }
  uint32_t c = *n < NFORMATS ? *n : NFORMATS;
  for (uint32_t i = 0; i < c; i++) out[i].surfaceFormat = kFormats[i];
  *n = c;
  return c < NFORMATS ? VK_INCOMPLETE : VK_SUCCESS;
}

static const VkPresentModeKHR kModes[] = { VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR };
#define NMODES ((uint32_t)(sizeof kModes / sizeof kModes[0]))

static VKAPI_ATTR VkResult VKAPI_CALL
c1_GetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice pd, VkSurfaceKHR s,
                                           uint32_t* n, VkPresentModeKHR* out) {
  (void)pd; (void)s;
  if (!n) return VK_ERROR_INITIALIZATION_FAILED;
  if (!out) { *n = NMODES; return VK_SUCCESS; }
  uint32_t c = *n < NMODES ? *n : NMODES;
  memcpy(out, kModes, c * sizeof kModes[0]);
  *n = c;
  return c < NMODES ? VK_INCOMPLETE : VK_SUCCESS;
}

// ---------------------------------------------------------------------------------- swapchain
static void flush_pending(struct c1_swapchain* sc);
static void frame_dist(struct c1_swapchain* sc, double out[4]);

static void sc_teardown(struct c1_swapchain* sc) {
  if (!sc) return;
  if (vk.DeviceWaitIdle && sc->dev) vk.DeviceWaitIdle(sc->dev);
  sc->pending = -1;                              // the device is idle; nothing left to wait on
  for (uint32_t i = 0; i < sc->n; i++) {
    if (sc->wbuf[i]) wl.proxy_marshal_flags(sc->wbuf[i], WL_BUFFER_DESTROY, NULL, 1, 1 /*DESTROY*/);
    if (sc->fence[i] && vk.DestroyFence) vk.DestroyFence(sc->dev, sc->fence[i], NULL);
    if (sc->img[i] && vk.DestroyImage) vk.DestroyImage(sc->dev, sc->img[i], NULL);
    if (sc->imem[i] && vk.FreeMem) vk.FreeMem(sc->dev, sc->imem[i], NULL);
  }
  if (sc->wlpool) wl.proxy_marshal_flags(sc->wlpool, WL_SHM_POOL_DESTROY, NULL, 1, 1);
  if (sc->q) wl.event_queue_destroy(sc->q);
  // THE VULKAN OBJECTS GO BEFORE THE munmap NOW. With the pool imported as VkDeviceMemory, the
  // old order (munmap, then vkFreeMemory) hands MoltenVK a freed mapping to release — a
  // use-after-unmap that the staging path could never have.
  if (sc->stage && vk.DestroyBuffer) vk.DestroyBuffer(sc->dev, sc->stage, NULL);
  if (sc->smem && vk.FreeMem) vk.FreeMem(sc->dev, sc->smem, NULL);
  if (sc->pool && vk.DestroyCmdPool) vk.DestroyCmdPool(sc->dev, sc->pool, NULL);
  if (sc->shmraw) munmap(sc->shmraw, sc->shmraw_size);
  free(sc);
}

static VKAPI_ATTR VkResult VKAPI_CALL
c1_CreateSwapchainKHR(VkDevice dev, const VkSwapchainCreateInfoKHR* ci,
                      const VkAllocationCallbacks* a, VkSwapchainKHR* out) {
  (void)a;
  if (!ci || !out) return VK_ERROR_INITIALIZATION_FAILED;
  if (!load_device_procs(dev)) { WARN("cannot resolve the device procs the swapchain needs");
                                 return VK_ERROR_INITIALIZATION_FAILED; }
  struct c1_surface* s = (struct c1_surface*)(uintptr_t)ci->surface;
  if (!s || s->magic != C1_SURFACE_MAGIC) {
    WARN("CreateSwapchainKHR with a surface this ICD did not create (%p)", (void*)s);
    return VK_ERROR_SURFACE_LOST_KHR;
  }
  struct c1_swapchain* sc = calloc(1, sizeof *sc);
  if (!sc) return VK_ERROR_OUT_OF_HOST_MEMORY;
  sc->magic = C1_SWAPCHAIN_MAGIC;
  sc->pending = -1;                              // calloc's 0 is a VALID image index, so say it
  sc->dev = dev; sc->s = s; sc->fmt = ci->imageFormat;
  sc->w = ci->imageExtent.width; sc->h = ci->imageExtent.height;
  // THREE, not two. With two buffers the renderer is stalled on the compositor for a whole frame
  // every frame (measured: the "wait for release" leg dominated everything else by 50x).
  sc->n = ci->minImageCount < 3 ? 3 : (ci->minImageCount > MAX_IMG ? MAX_IMG : ci->minImageCount);
  // C1WSI_IMAGES=4 — a HYPOTHESIS UNDER TEST, not a setting. Deferring the present costs one slot
  // of slack: the client is a frame further ahead, so the slot it wants back was attached two
  // frames ago instead of three. MEASURED at 1024x1024, that shows up as wait-for-release growing
  // by ~0.22 ms, which is almost exactly the 0.31 ms of fence wait the deferral removed — i.e. the
  // time did not vanish, it moved. If that is the whole story, a fourth image should give it back.
  { const char* e = getenv("C1WSI_IMAGES");
    if (e) { unsigned v = (unsigned)atoi(e); if (v >= 2 && v <= MAX_IMG) sc->n = v; } }
  if (!sc->w || !sc->h) { free(sc); return VK_ERROR_INITIALIZATION_FAILED; }
  sc->stride = (size_t)sc->w * 4;                // pool geometry; zero-copy may widen it below
  sc->slot   = sc->stride * sc->h;

  VkResult r;

  // ---------------------------------------------------------------------------- ZERO COPY
  // C1WSI_ZEROCOPY=1. The strongest form of this file: make the swapchain images LINEAR and bind
  // them INTO the wl_shm pool, so zink's render target IS the compositor's buffer. Then present
  // issues no blit, records no command buffer and moves no bytes — the 0.224 ms fixed round trip
  // and the 0.0155 ms/MiB slope both stop existing rather than being deferred.
  //
  // A PROBE DECIDES IT, not an assumption, and it runs BEFORE anything is allocated because the
  // pool's geometry has to come from the image rather than the other way round. Three things must
  // hold; any one of them missing falls back to the ordinary optimal-tiling + blit path, which is
  // why this is safe to ask for unconditionally:
  //   1. MoltenVK accepts TILING_LINEAR with the usage zink asked for;
  //   2. rowPitch is exactly width*4, so a wl_shm buffer describes the image with no repacking;
  //   3. the slot stride satisfies the image's own alignment.
  // MEASURED on M2 Max / MoltenVK 1.4 at 512x512: all three hold (rowPitch 2048 == width*4,
  // alignment 16), and vkBindImageMemory into the imported pool returns VK_SUCCESS.
  if (getenv("C1WSI_ZEROCOPY") && !g_noimport) {
    VkImageCreateInfo pi2 = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    pi2.imageType = VK_IMAGE_TYPE_2D; pi2.format = sc->fmt;
    pi2.extent.width = sc->w; pi2.extent.height = sc->h; pi2.extent.depth = 1;
    pi2.mipLevels = 1; pi2.arrayLayers = 1; pi2.samples = VK_SAMPLE_COUNT_1_BIT;
    pi2.tiling = VK_IMAGE_TILING_LINEAR;
    pi2.usage = ci->imageUsage;
    pi2.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    pi2.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage probe = 0;
    if (vk.CreateImage(dev, &pi2, NULL, &probe) == VK_SUCCESS) {
      VkMemoryRequirements mr; vk.ImageMemReq(dev, probe, &mr);
      PFN_vkGetImageSubresourceLayout gsl =
        (PFN_vkGetImageSubresourceLayout)vk.gdpa(dev, "vkGetImageSubresourceLayout");
      VkSubresourceLayout sl = { 0 };
      if (gsl) { VkImageSubresource sub = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 }; gsl(dev, probe, &sub, &sl); }
      size_t al = mr.alignment ? (size_t)mr.alignment : 1;
      if (gsl && sl.rowPitch == (VkDeviceSize)sc->w * 4) {
        sc->zc = 1;
        sc->stride = (size_t)sl.rowPitch;
        sc->slot = ((size_t)mr.size + al - 1) / al * al;
        if (sc->slot < sc->stride * sc->h) sc->slot = sc->stride * sc->h;
        LOG("ZEROCOPY: linear image accepted, rowPitch=%zu slot=%zu align=%zu — the swapchain "
            "images will live IN the wl_shm pool", sc->stride, sc->slot, al);
      } else {
        WARN("ZEROCOPY declined: rowPitch=%llu but width*4=%u — a wl_shm buffer cannot describe "
             "that without repacking; using the blit path",
             (unsigned long long)sl.rowPitch, sc->w * 4);
      }
      vk.DestroyImage(dev, probe, NULL);
    } else {
      WARN("ZEROCOPY declined: vkCreateImage(LINEAR, usage=0x%x) refused; using the blit path",
           ci->imageUsage);
    }
  }

  for (uint32_t i = 0; sc->zc ? 0 : i < sc->n; i++) {
    VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = sc->fmt;
    ii.extent.width = sc->w; ii.extent.height = sc->h; ii.extent.depth = 1;
    ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = ci->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if ((r = vk.CreateImage(dev, &ii, NULL, &sc->img[i])) != VK_SUCCESS) {
      WARN("vkCreateImage(%u) -> %d", i, r); sc_teardown(sc); return r; }
    VkMemoryRequirements mr; vk.ImageMemReq(dev, sc->img[i], &mr);
    uint32_t t = pick_memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (t == UINT32_MAX) t = pick_memtype(mr.memoryTypeBits, 0,
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                .allocationSize = mr.size, .memoryTypeIndex = t };
    if ((r = vk.AllocMem(dev, &ai, NULL, &sc->imem[i])) != VK_SUCCESS) {
      WARN("vkAllocateMemory(image %u, %llu bytes, type %u) -> %d", i,
           (unsigned long long)mr.size, t, r); sc_teardown(sc); return r; }
    if ((r = vk.BindImage(dev, sc->img[i], sc->imem[i], 0)) != VK_SUCCESS) {
      WARN("vkBindImageMemory(%u) -> %d", i, r); sc_teardown(sc); return r; }
  }

  // THE WAYLAND POOL IS CREATED FIRST NOW, because the destination buffer is (when it can be) the
  // pool itself. Nothing below depends on the ordering the other way round.
  if (!wl_setup(sc)) { sc_teardown(sc); return VK_ERROR_INITIALIZATION_FAILED; }

  // ------------------------------------------------------------------- the readback destination
  //
  // FIRST CHOICE — no destination at all. On Apple silicon "GPU memory" and "CPU memory" are the
  // same DRAM, so copying the frame into a driver-allocated staging buffer and then memcpy'ing
  // that into the wl_shm pool is a copy between two addresses in one RAM, for no reason other
  // than that the two allocations were made by different allocators. VK_EXT_external_memory_host
  // lets MoltenVK adopt the pool's OWN pages as a VkDeviceMemory (Metal:
  // newBufferWithBytesNoCopy:), so vkCmdCopyImageToBuffer lands the frame directly in the pages
  // the compositor reads. The memcpy then does not get faster; it stops existing.
  //
  // MEASURED, not assumed: memcpy->shm was 0.030 ms per MiB (0.128 ms at 1024x1024, 0.010 ms at
  // 300x300) on the staging path. C1WSI_NOIMPORT=1 puts the staging buffer back so the difference
  // can be shown going away.
  //
  // The buffer spans the WHOLE POOL (all n slots) in both modes, not one frame. That is what lets
  // the present be deferred: a one-frame staging buffer would have frame N+1's copy overwrite
  // frame N's bytes before frame N was posted.
  VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
  bi.size = (VkDeviceSize)sc->shmsize;
  bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  LOG("import preconditions: noimport=%d HostPtrProps=%p align=%zu ptr=%p ptr%%align=%zu",
      g_noimport, (void*)vk.HostPtrProps, g_import_align, (void*)sc->shmmap,
      g_import_align ? (size_t)((uintptr_t)sc->shmmap % g_import_align) : 0);
  if (!g_noimport && vk.HostPtrProps && g_import_align &&
      ((uintptr_t)sc->shmmap % g_import_align) == 0) {
    // NO VkExternalMemoryBufferCreateInfo ON THE BUFFER. Chaining it is what the spec's example
    // does, and MoltenVK 1.4 REJECTS it: vkCreateBuffer returns -8 (VK_ERROR_FEATURE_NOT_PRESENT)
    // for handleTypes=HOST_ALLOCATION, because MoltenVK only accepts MTLBUFFER for a buffer's
    // external handle types. MEASURED — the first version of this chained it, silently fell back
    // to staging, and the verbose log said "import unavailable" while every precondition was met.
    // The import is a property of the MEMORY, not of the buffer, and binding ordinary-buffer to
    // imported-memory is what actually works here.
    VkBufferCreateInfo ebi = bi;
    VkMemoryHostPointerPropertiesEXT hp = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
    VkResult hr = vk.HostPtrProps(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                  sc->shmmap, &hp);
    VkResult br = VK_ERROR_UNKNOWN;
    if (hr == VK_SUCCESS && hp.memoryTypeBits)
      br = vk.CreateBuffer(dev, &ebi, NULL, &sc->stage);
    LOG("vkGetMemoryHostPointerPropertiesEXT -> %d memoryTypeBits=0x%x  CreateBuffer -> %d",
        hr, hp.memoryTypeBits, br);
    if (br == VK_SUCCESS) {
      VkMemoryRequirements bmr; vk.BufferMemReq(dev, sc->stage, &bmr);
      uint32_t bt = pick_memtype(bmr.memoryTypeBits & hp.memoryTypeBits,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
      if (bt == UINT32_MAX)
        bt = pick_memtype(bmr.memoryTypeBits & hp.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0);
      VkImportMemoryHostPointerInfoEXT imp = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
        .pHostPointer = sc->shmmap };
      VkMemoryAllocateInfo iai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                   .pNext = &imp,
                                   .allocationSize = (VkDeviceSize)sc->shmsize,
                                   .memoryTypeIndex = bt };
      VkResult ar = bt == UINT32_MAX ? VK_ERROR_UNKNOWN : vk.AllocMem(dev, &iai, NULL, &sc->smem);
      VkResult kr = ar == VK_SUCCESS ? vk.BindBuffer(dev, sc->stage, sc->smem, 0) : ar;
      LOG("import: bufMemBits=0x%x hostBits=0x%x -> type %u  AllocateMemory -> %d  Bind -> %d",
          bmr.memoryTypeBits, hp.memoryTypeBits, bt, ar, kr);
      if (kr == VK_SUCCESS) {
        sc->imported = 1;
        sc->smap = sc->shmmap;                   // same pages; the probe below still works
        LOG("wl_shm pool IMPORTED as VkDeviceMemory (%zu bytes, type %u) — no memcpy on present",
            sc->shmsize, bt);
      } else {
        WARN("host-pointer import failed (memtype %u); falling back to a staging buffer", bt);
        if (sc->smem) { vk.FreeMem(dev, sc->smem, NULL); sc->smem = 0; }
        vk.DestroyBuffer(dev, sc->stage, NULL); sc->stage = 0;
      }
    } else if (sc->stage) { vk.DestroyBuffer(dev, sc->stage, NULL); sc->stage = 0; }
  }

  // ZERO COPY, part two: the images go IN the pool. This is after the import because it binds to
  // the imported VkDeviceMemory. If anything here refuses, we fall all the way back — including
  // re-creating the ordinary optimal-tiling images — rather than shipping a half-built swapchain.
  if (sc->zc) {
    int ok = sc->imported;
    if (!ok) WARN("ZEROCOPY: the pool could not be imported, so there is nothing to bind into");
    for (uint32_t i = 0; ok && i < sc->n; i++) {
      VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
      ii.imageType = VK_IMAGE_TYPE_2D; ii.format = sc->fmt;
      ii.extent.width = sc->w; ii.extent.height = sc->h; ii.extent.depth = 1;
      ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT;
      ii.tiling = VK_IMAGE_TILING_LINEAR;
      ii.usage = ci->imageUsage;
      ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      if (vk.CreateImage(dev, &ii, NULL, &sc->img[i]) != VK_SUCCESS) { ok = 0; break; }
      VkResult br = vk.BindImage(dev, sc->img[i], sc->smem, (VkDeviceSize)(sc->slot * i));
      if (br != VK_SUCCESS) { WARN("ZEROCOPY: vkBindImageMemory(slot %u) -> %d", i, br); ok = 0; }
    }
    if (!ok) {
      WARN("ZEROCOPY: bind failed — falling back to the blit path for this swapchain");
      for (uint32_t i = 0; i < sc->n; i++)
        if (sc->img[i]) { vk.DestroyImage(dev, sc->img[i], NULL); sc->img[i] = 0; }
      sc->zc = 0;
      for (uint32_t i = 0; i < sc->n; i++) {
        VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ii.imageType = VK_IMAGE_TYPE_2D; ii.format = sc->fmt;
        ii.extent.width = sc->w; ii.extent.height = sc->h; ii.extent.depth = 1;
        ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = ci->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if ((r = vk.CreateImage(dev, &ii, NULL, &sc->img[i])) != VK_SUCCESS) {
          sc_teardown(sc); return r; }
        VkMemoryRequirements mr; vk.ImageMemReq(dev, sc->img[i], &mr);
        uint32_t t = pick_memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (t == UINT32_MAX) t = pick_memtype(mr.memoryTypeBits, 0,
                                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                    .allocationSize = mr.size, .memoryTypeIndex = t };
        if ((r = vk.AllocMem(dev, &ai, NULL, &sc->imem[i])) != VK_SUCCESS) {
          sc_teardown(sc); return r; }
        if ((r = vk.BindImage(dev, sc->img[i], sc->imem[i], 0)) != VK_SUCCESS) {
          sc_teardown(sc); return r; }
      }
    } else {
      LOG("ZEROCOPY ACTIVE: %u swapchain images bound into the wl_shm pool — present does no blit",
          sc->n);
    }
  }

  if (!sc->imported) {
    if ((r = vk.CreateBuffer(dev, &bi, NULL, &sc->stage)) != VK_SUCCESS) {
      WARN("vkCreateBuffer -> %d", r); sc_teardown(sc); return r; }
    VkMemoryRequirements bmr; vk.BufferMemReq(dev, sc->stage, &bmr);
    // The staging BUFFER is the opposite case to the images: buffers have no tiling, so
    // host-coherent is exactly what is wanted and is legal here.
    uint32_t bt = pick_memtype(bmr.memoryTypeBits,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    if (bt == UINT32_MAX) bt = pick_memtype(bmr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0);
    VkMemoryAllocateInfo bai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = bmr.size, .memoryTypeIndex = bt };
    if ((r = vk.AllocMem(dev, &bai, NULL, &sc->smem)) != VK_SUCCESS) {
      WARN("vkAllocateMemory(staging) -> %d", r); sc_teardown(sc); return r; }
    if ((r = vk.BindBuffer(dev, sc->stage, sc->smem, 0)) != VK_SUCCESS) {
      WARN("vkBindBufferMemory -> %d", r); sc_teardown(sc); return r; }
    if ((r = vk.MapMemory(dev, sc->smem, 0, VK_WHOLE_SIZE, 0, (void**)&sc->smap)) != VK_SUCCESS) {
      WARN("vkMapMemory -> %d", r); sc_teardown(sc); return r; }
    LOG("staging buffer + memcpy (import %s)",
        g_noimport ? "disabled by C1WSI_NOIMPORT" : "unavailable");
  }

  // ---------------------------------------------------------- DOES THE READBACK NEED TO EXIST?
  // C1WSI_ZEROCOPY=1 is a PROBE, not a feature. The strongest possible version of this file has no
  // readback at all: make the swapchain images LINEAR-tiling and bind them directly to the
  // imported wl_shm pool, and zink renders straight into the pages the compositor reads — no blit,
  // no fence, no present-time GPU work whatsoever. Whether Metal permits that is a question about
  // Metal, and the honest way to answer it is to ask it rather than to reason about it, so this
  // asks and prints the answer. It binds nothing and changes no behaviour.
  //
  // The three things that have to be true, checked in order, because each one can veto:
  //   1. vkCreateImage accepts TILING_LINEAR with the usage zink asked for (COLOR_ATTACHMENT);
  //   2. its memoryTypeBits include the type the imported pool memory actually has;
  //   3. its rowPitch and required alignment are ones a wl_shm buffer can describe.
  if (getenv("C1WSI_ZEROCOPY") && sc->imported) {
    VkImageCreateInfo li = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    li.imageType = VK_IMAGE_TYPE_2D; li.format = sc->fmt;
    li.extent.width = sc->w; li.extent.height = sc->h; li.extent.depth = 1;
    li.mipLevels = 1; li.arrayLayers = 1; li.samples = VK_SAMPLE_COUNT_1_BIT;
    li.tiling = VK_IMAGE_TILING_LINEAR;
    li.usage = ci->imageUsage;
    li.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    li.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage probe = 0;
    VkResult pr = vk.CreateImage(dev, &li, NULL, &probe);
    WARN("ZEROCOPY probe: vkCreateImage(LINEAR, usage=0x%x) -> %d", ci->imageUsage, pr);
    if (pr == VK_SUCCESS) {
      VkMemoryRequirements pmr; vk.ImageMemReq(dev, probe, &pmr);
      VkMemoryRequirements bmr2; vk.BufferMemReq(dev, sc->stage, &bmr2);
      PFN_vkGetImageSubresourceLayout gsl =
        (PFN_vkGetImageSubresourceLayout)vk.gdpa(dev, "vkGetImageSubresourceLayout");
      VkSubresourceLayout sl = { 0 };
      if (gsl) { VkImageSubresource sub = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 }; gsl(dev, probe, &sub, &sl); }
      WARN("ZEROCOPY probe: image memBits=0x%x align=%llu size=%llu rowPitch=%llu (want %u) | "
           "pool memBits=0x%x  -> bindable=%s",
           pmr.memoryTypeBits, (unsigned long long)pmr.alignment,
           (unsigned long long)pmr.size, (unsigned long long)sl.rowPitch, sc->w * 4,
           bmr2.memoryTypeBits,
           (pmr.memoryTypeBits & bmr2.memoryTypeBits) ? "memtype OK" : "NO COMMON MEMORY TYPE");
      // The one that actually decides it: will MoltenVK bind this image to the imported pool?
      VkResult br2 = vk.BindImage(dev, probe, sc->smem, 0);
      WARN("ZEROCOPY probe: vkBindImageMemory(image, imported pool, offset 0) -> %d", br2);
      vk.DestroyImage(dev, probe, NULL);
    }
  }

  VkCommandPoolCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
  pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pi.queueFamilyIndex = 0;                     // queueCount=1 on one family: there is no choice
  if ((r = vk.CreateCmdPool(dev, &pi, NULL, &sc->pool)) != VK_SUCCESS) {
    WARN("vkCreateCommandPool -> %d", r); sc_teardown(sc); return r; }
  VkCommandBufferAllocateInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
  cbi.commandPool = sc->pool; cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbi.commandBufferCount = sc->n;
  if ((r = vk.AllocCmdBufs(dev, &cbi, sc->cb)) != VK_SUCCESS) {
    WARN("vkAllocateCommandBuffers -> %d", r); sc_teardown(sc); return r; }
  for (uint32_t i = 0; i < sc->n; i++) {
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if ((r = vk.CreateFence(dev, &fi, NULL, &sc->fence[i])) != VK_SUCCESS) {
      WARN("vkCreateFence -> %d", r); sc_teardown(sc); return r; }
  }

  *out = (VkSwapchainKHR)(uintptr_t)sc;
  LOG("CreateSwapchainKHR %ux%u fmt=%d images=%u -> %p", sc->w, sc->h, (int)sc->fmt, sc->n, (void*)sc);
  return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
c1_DestroySwapchainKHR(VkDevice dev, VkSwapchainKHR h, const VkAllocationCallbacks* a) {
  (void)dev; (void)a;
  struct c1_swapchain* sc = (struct c1_swapchain*)(uintptr_t)h;
  if (!sc || sc->magic != C1_SWAPCHAIN_MAGIC) return;
  flush_pending(sc);                             // do not drop the last frame on the floor
  double dist[4]; frame_dist(sc, dist);
  if (g_stats && sc->presents)
    fprintf(stderr, "[c1wsi] %llu presents/frame: record+submit %.3f ms  gpu-readback %.3f ms  "
            "wait-for-release %.3f ms  memcpy->shm %.3f ms  |  FRAME mean %.3f min %.3f p50 %.3f "
            "p95 %.3f ms (n=%llu)  [%s%s]\n",
            (unsigned long long)sc->presents,
            (double)sc->ns_submit / sc->presents / 1e6,
            (double)sc->ns_gpu / sc->presents / 1e6, (double)sc->ns_wait / sc->presents / 1e6,
            (double)sc->ns_copy / sc->presents / 1e6,
            sc->frames_timed ? (double)sc->ns_frame / sc->frames_timed / 1e6 : 0.0,
            dist[0], dist[1], dist[2],
            (unsigned long long)sc->frames_timed,
            sc->zc ? "ZEROCOPY" : sc->imported ? "imported" : "staged",
            g_nodefer ? ",blocking" : ",deferred");
  sc->magic = 0;
  sc_teardown(sc);
}

static VKAPI_ATTR VkResult VKAPI_CALL
c1_GetSwapchainImagesKHR(VkDevice dev, VkSwapchainKHR h, uint32_t* n, VkImage* out) {
  (void)dev;
  struct c1_swapchain* sc = (struct c1_swapchain*)(uintptr_t)h;
  if (!sc || sc->magic != C1_SWAPCHAIN_MAGIC || !n) return VK_ERROR_INITIALIZATION_FAILED;
  if (!out) { *n = sc->n; return VK_SUCCESS; }
  uint32_t c = *n < sc->n ? *n : sc->n;
  for (uint32_t i = 0; i < c; i++) out[i] = sc->img[i];
  *n = c;
  return c < sc->n ? VK_INCOMPLETE : VK_SUCCESS;
}

// Signalling a BINARY semaphore is only possible from a queue submit — Vulkan has no host-side
// signal for one (vkSignalSemaphore is timeline-only). So acquire submits an empty batch on the
// cached queue. See the single-queue note at the top of this file.
static VkResult signal_acquire(VkDevice dev, VkSemaphore sem, VkFence fence) {
  if (!sem && !fence) return VK_SUCCESS;
  if (!g_queue) { WARN("acquire must signal, but no VkQueue has been seen yet"); return VK_SUCCESS; }
  VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
  if (sem) { si.signalSemaphoreCount = 1; si.pSignalSemaphores = &sem; }
  (void)dev;
  return vk.QueueSubmit(g_queue, 1, &si, fence);
}

static VKAPI_ATTR VkResult VKAPI_CALL
c1_AcquireNextImageKHR(VkDevice dev, VkSwapchainKHR h, uint64_t timeout,
                       VkSemaphore sem, VkFence fence, uint32_t* idx) {
  (void)timeout;
  struct c1_swapchain* sc = (struct c1_swapchain*)(uintptr_t)h;
  if (!sc || sc->magic != C1_SWAPCHAIN_MAGIC || !idx) return VK_ERROR_OUT_OF_DATE_KHR;
  uint32_t i = sc->next % sc->n;
  sc->next++;
  // THE INVARIANT, enforced rather than argued. With n>=3 and at most one deferred present, the
  // image handed out here is never the one whose readback is still in flight. If a driver ever
  // asks for a 2-image swapchain, or the deferral depth grows, this line keeps the frame correct
  // instead of racing the copy — at the cost of the stall the deferral was avoiding.
  if (sc->pending == (int)i) flush_pending(sc);
  *idx = i;
  return signal_acquire(dev, sem, fence) == VK_SUCCESS ? VK_SUCCESS : VK_SUCCESS;
}

typedef struct { VkStructureType sType; const void* pNext; VkSwapchainKHR swapchain;
                 uint64_t timeout; VkSemaphore semaphore; VkFence fence;
                 uint32_t deviceMask; } C1AcquireInfo2;

static VKAPI_ATTR VkResult VKAPI_CALL
c1_AcquireNextImage2KHR(VkDevice dev, const C1AcquireInfo2* ai, uint32_t* idx) {
  if (!ai) return VK_ERROR_OUT_OF_DATE_KHR;
  return c1_AcquireNextImageKHR(dev, ai->swapchain, ai->timeout, ai->semaphore, ai->fence, idx);
}

// Post the frame whose readback was submitted earlier: wait for the GPU to have finished it,
// memcpy it into the pool if it did not land there directly, then attach/damage/commit.
//
// WHY THIS IS A SEPARATE FUNCTION, i.e. why the fence wait moved out of present.
// MEASURED on the old shape: gpu-readback was 0.227 ms at 128x128, 0.227 ms at 300x300, 0.251 ms
// at 512x512, 0.289 ms at 1024x1024 — a 64x range of pixels for a 27% range of time. That leg was
// never the copy; it was a ~0.22 ms fixed CPU<->GPU round trip, submitted and then blocked on,
// once per frame, on the one queue MoltenVK gives us. The copy itself is the 0.0155 ms/MiB slope,
// which is ~64 GB/s and entirely reasonable.
//
// You cannot make that round trip faster. You can stop waiting inside it: submit frame N's
// readback, return to the client so zink can record and submit frame N+1, and only then wait for
// frame N — by which time the GPU has usually finished and the wait is a poll. The cost is one
// frame of latency, which is what a swapchain is for. C1WSI_NODEFER=1 restores the blocking shape
// so the difference can be shown going away.
//
// It is safe with n>=3 images: acquire hands out next%n, and at most ONE image is ever pending,
// so the image the client is about to render into is never the one still being read back. The
// assert below is not decoration — it is the invariant the whole scheme rests on.
static int cmp_u32(const void* a, const void* b) {
  uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
  return x < y ? -1 : x > y ? 1 : 0;
}

// min / p50 / p95 / mean of the frame period, in ms. A mean alone cannot say whether a change
// shifted the centre or shaved a tail, and that ambiguity is exactly what a cross-harness
// disagreement turns into an argument about.
static void frame_dist(struct c1_swapchain* sc, double out[4]) {
  out[0] = out[1] = out[2] = out[3] = 0.0;
  if (!sc->nsamp) return;
  uint32_t* v = malloc((size_t)sc->nsamp * sizeof *v);
  if (!v) return;
  memcpy(v, sc->samp_us10, (size_t)sc->nsamp * sizeof *v);
  qsort(v, sc->nsamp, sizeof *v, cmp_u32);
  out[0] = v[0] / 10000.0;
  out[1] = v[sc->nsamp / 2] / 10000.0;
  out[2] = v[(uint32_t)((double)sc->nsamp * 0.95)] / 10000.0;
  double sum = 0; for (uint32_t i = 0; i < sc->nsamp; i++) sum += v[i];
  out[3] = sum / sc->nsamp / 10000.0;
  free(v);
}

static void flush_pending(struct c1_swapchain* sc) {
  if (sc->pending < 0) return;
  uint32_t p = (uint32_t)sc->pending;
  sc->pending = -1;

  uint64_t t0 = now_ns();
  VkResult r = vk.WaitFences(sc->dev, 1, &sc->fence[p], VK_TRUE, 2000000000ull);
  uint64_t t1 = now_ns();
  if (r != VK_SUCCESS) { WARN("present fence wait -> %d", r); return; }

  size_t stride = sc->stride;
  size_t off = sc->slot * p;
  if (!sc->imported) memcpy(sc->shmmap + off, sc->smap + off, stride * sc->h);
  uint64_t t2 = now_ns();

  // WHERE DID THE PIXELS GO. The first version of this file presented a perfectly black window:
  // every call succeeded, frames reached the compositor, and the image was empty. Distinguishing
  // "the GPU copied nothing" from "we posted the wrong bytes" needs a probe on what was staged
  // BEFORE it is posted, so it is here rather than reconstructed afterwards from the PPM.
  if (g_v && sc->presents < 3) {
    const uint8_t* q = sc->shmmap + off;
    size_t nz = 0, n = stride * sc->h;
    for (size_t i = 0; i < n; i++) if (q[i]) nz++;
    LOG("staged image %u: %zu/%zu nonzero bytes, first px = %02x %02x %02x %02x",
        p, nz, n, q[0], q[1], q[2], q[3]);
  }

  if (sc->s->headless) {                            // the frame is complete; there is nowhere to put it
    sc->ns_gpu += t1 - t0;
    sc->ns_copy += t2 - t1;
    return;
  }

  struct wl_proxy* surfp = (struct wl_proxy*)sc->s->surf;
  uint32_t sv = wl.proxy_get_version(surfp);
  sc->released[p] = 0;
  wl.proxy_marshal_flags(surfp, WL_SURFACE_ATTACH, NULL, sv, 0, sc->wbuf[p], 0, 0);
  wl.proxy_marshal_flags(surfp, WL_SURFACE_DAMAGE, NULL, sv, 0, 0, 0,
                         (int32_t)sc->w, (int32_t)sc->h);
  wl.proxy_marshal_flags(surfp, WL_SURFACE_COMMIT, NULL, sv, 0);
  wl.display_flush(sc->s->dpy);

  sc->ns_gpu += t1 - t0;
  sc->ns_copy += t2 - t1;
}

static VkResult present_one(VkQueue queue, struct c1_swapchain* sc, uint32_t idx,
                            const VkSemaphore* waits, uint32_t nwait) {
  VkResult r;
  // THE RELEASE WAIT MOVED IN FRONT OF THE SUBMIT. With the pool imported, the GPU writes into the
  // very pages the compositor may still be reading, so "wait, then copy" has to become "wait, then
  // let the GPU copy". On the staging path it is merely harmless. Either way this is normally free:
  // with three buffers the compositor has released this slot two frames ago.
  uint64_t tp = now_ns();
  if (sc->last_present_ns && sc->presents > 30) {   // 30 = past Metal's shader compiles
    uint64_t d = tp - sc->last_present_ns;
    sc->ns_frame += d; sc->frames_timed++;
    if (sc->nsamp < sizeof sc->samp_us10 / sizeof sc->samp_us10[0]) {
      uint64_t t = d / 100;                        // ns -> tenths of a microsecond
      sc->samp_us10[sc->nsamp++] = t > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)t;
    }
  }
  sc->last_present_ns = tp;

  // POST THE PREVIOUS FRAME FIRST. It was submitted a whole client frame ago, so its fence is
  // almost certainly signalled and this is a poll; doing it here rather than after this frame's
  // submit hands the compositor its work as early as possible instead of holding it back behind
  // our own command recording.
  if (!g_nodefer) flush_pending(sc);

  uint64_t tw0 = now_ns();
  if (!sc->s->headless)
    for (int spin = 0; !sc->released[idx] && spin < 200; spin++)
      wl.display_roundtrip_queue(sc->s->dpy, sc->q);
  sc->ns_wait += now_ns() - tw0;

  uint64_t t0 = now_ns();

  // ZERO COPY: there is nothing to record. zink already rendered into the pool. All that is still
  // needed is a point in time at which the GPU is known to have FINISHED — and Vulkan gives no
  // host-side wait on a binary semaphore, so an EMPTY batch carrying the present's wait semaphores
  // and signalling our fence is the cheapest legal way to get one. No command buffer, no barrier,
  // no blit, no bytes moved. Combined with the deferral, even that fence wait is a poll.
  if (sc->zc) {
    VkPipelineStageFlags zstages[8];
    for (uint32_t i = 0; i < nwait && i < 8; i++) zstages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo zsi = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
    zsi.waitSemaphoreCount = nwait > 8 ? 8 : nwait;
    zsi.pWaitSemaphores = waits;
    zsi.pWaitDstStageMask = zstages;
    vk.ResetFences(sc->dev, 1, &sc->fence[idx]);
    if ((r = vk.QueueSubmit(queue, 1, &zsi, sc->fence[idx])) != VK_SUCCESS) {
      WARN("zero-copy present submit -> %d", r); return r; }
    sc->ns_submit += now_ns() - t0;
    sc->presents++;
    sc->pending = (int)idx;
    if (g_nodefer) flush_pending(sc);
    goto zc_report;
  }

  VkCommandBuffer cb = sc->cb[idx];
  vk.ResetCmd(cb, 0);
  VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                  .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
  if ((r = vk.BeginCmd(cb, &bi)) != VK_SUCCESS) return r;

  VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
  b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = sc->img[idx];
  b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  b.subresourceRange.levelCount = 1; b.subresourceRange.layerCount = 1;
  vk.CmdBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                0, NULL, 0, NULL, 1, &b);

  VkBufferImageCopy c = { 0 };
  c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  c.imageSubresource.layerCount = 1;
  c.imageExtent.width = sc->w; c.imageExtent.height = sc->h; c.imageExtent.depth = 1;
  // The destination is the pool slot for THIS image, not the start of a one-frame buffer: the
  // buffer now spans all n slots (imported = it IS the pool).
  c.bufferOffset = (VkDeviceSize)((size_t)sc->w * 4 * sc->h * idx);
  vk.CmdCopyImageToBuffer(cb, sc->img[idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc->stage, 1, &c);

  b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  vk.CmdBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                0, NULL, 0, NULL, 1, &b);
  if ((r = vk.EndCmd(cb)) != VK_SUCCESS) return r;

  VkPipelineStageFlags stages[8];
  for (uint32_t i = 0; i < nwait && i < 8; i++) stages[i] = VK_PIPELINE_STAGE_TRANSFER_BIT;
  VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
  si.waitSemaphoreCount = nwait > 8 ? 8 : nwait;
  si.pWaitSemaphores = waits;
  si.pWaitDstStageMask = stages;
  si.commandBufferCount = 1; si.pCommandBuffers = &cb;
  vk.ResetFences(sc->dev, 1, &sc->fence[idx]);
  if ((r = vk.QueueSubmit(queue, 1, &si, sc->fence[idx])) != VK_SUCCESS) {
    WARN("present submit -> %d", r); return r; }
  sc->ns_submit += now_ns() - t0;
  sc->presents++;

  sc->pending = (int)idx;
  if (g_nodefer) flush_pending(sc);              // the old, blocking shape

zc_report:
  // Report PERIODICALLY, not only at vkDestroySwapchainKHR. An event-loop app is killed, never
  // destroyed, so an end-of-run report is a report that never prints -- and a missing number reads
  // as "cheap" rather than "unmeasured".
  if (g_stats && sc->presents % 100 == 0) {
    double dist[4]; frame_dist(sc, dist);
    fprintf(stderr, "[c1wsi] %llu presents/frame: record+submit %.3f ms  gpu-readback %.3f ms  "
            "wait-for-release %.3f ms  memcpy->shm %.3f ms  |  FRAME mean %.3f min %.3f p50 %.3f "
            "p95 %.3f ms (n=%llu)  [%s%s]\n",
            (unsigned long long)sc->presents,
            (double)sc->ns_submit / sc->presents / 1e6,
            (double)sc->ns_gpu / sc->presents / 1e6, (double)sc->ns_wait / sc->presents / 1e6,
            (double)sc->ns_copy / sc->presents / 1e6,
            sc->frames_timed ? (double)sc->ns_frame / sc->frames_timed / 1e6 : 0.0,
            dist[0], dist[1], dist[2],
            (unsigned long long)sc->frames_timed,
            sc->zc ? "ZEROCOPY" : sc->imported ? "imported" : "staged",
            g_nodefer ? ",blocking" : ",deferred");
  }
  if (g_v && sc->presents <= 3) LOG("present #%llu image %u", (unsigned long long)sc->presents, idx);
  return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
c1_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pi) {
  if (!pi) return VK_ERROR_INITIALIZATION_FAILED;
  if (!g_queue) g_queue = queue;
  VkResult worst = VK_SUCCESS;
  for (uint32_t i = 0; i < pi->swapchainCount; i++) {
    struct c1_swapchain* sc = (struct c1_swapchain*)(uintptr_t)pi->pSwapchains[i];
    if (!sc || sc->magic != C1_SWAPCHAIN_MAGIC) { worst = VK_ERROR_SURFACE_LOST_KHR; continue; }
    VkResult r = present_one(queue, sc, pi->pImageIndices[i],
                             pi->pWaitSemaphores, pi->waitSemaphoreCount);
    if (pi->pResults) pi->pResults[i] = r;
    if (r != VK_SUCCESS) worst = r;
  }
  return worst;
}


// ============================================================================ draw probe
// C1WSI_TRACE=1 counts the calls that decide whether a frame CONTAINS anything, because "the
// window is the clear colour and nothing else" has two completely different causes and no log
// distinguishes them: either zink never issued the draw, or it issued it and the GPU dropped it.
//
// It is a counter with a NEGATIVE CONTROL built into how it is used: run it on the swrast+zink
// path, which is known to draw correctly (its glReadPixels matches llvmpipe exactly), and the
// counts must be non-zero there. An instrument that reads zero on the working configuration is
// measuring nothing — which is exactly what happened to the first attempt at this, a grep for
// MoltenVK's "Compiling Metal shader" line that reported 0 for the good path too.
static struct {
  PFN_vkCreateGraphicsPipelines cgp;
  PFN_vkCmdDraw draw;
  PFN_vkCmdDrawIndexed drawi;
  PFN_vkCmdBeginRenderPass brp;
  unsigned long n_cgp, n_cgp_fail, n_draw, n_drawi, n_brp;
} probe;
static int g_trace;

static VKAPI_ATTR VkResult VKAPI_CALL
p_CreateGraphicsPipelines(VkDevice d, VkPipelineCache c, uint32_t n,
                          const VkGraphicsPipelineCreateInfo* ci,
                          const VkAllocationCallbacks* a, VkPipeline* out) {
  VkResult r = probe.cgp(d, c, n, ci, a, out);
  probe.n_cgp += n;
  if (r != VK_SUCCESS) probe.n_cgp_fail += n;
  return r;
}
static VKAPI_ATTR void VKAPI_CALL
p_CmdDraw(VkCommandBuffer cb, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) {
  probe.n_draw++; probe.draw(cb, vc, ic, fv, fi);
}
static VKAPI_ATTR void VKAPI_CALL
p_CmdDrawIndexed(VkCommandBuffer cb, uint32_t ic, uint32_t inst, uint32_t fi, int32_t vo, uint32_t fin) {
  probe.n_drawi++; probe.drawi(cb, ic, inst, fi, vo, fin);
}
static VKAPI_ATTR void VKAPI_CALL
p_CmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo* bi, VkSubpassContents c) {
  probe.n_brp++;
  if (g_v && probe.n_brp <= 4)
    LOG("BeginRenderPass #%lu area %ux%u at (%d,%d)", probe.n_brp,
        bi->renderArea.extent.width, bi->renderArea.extent.height,
        bi->renderArea.offset.x, bi->renderArea.offset.y);
  probe.brp(cb, bi, c);
}
void c1wsi_report(void) {
  if (!g_trace) return;
  fprintf(stderr, "[c1wsi] PROBE: vkCreateGraphicsPipelines=%lu (failed %lu)  vkCmdDraw=%lu  "
          "vkCmdDrawIndexed=%lu  vkCmdBeginRenderPass=%lu\n",
          probe.n_cgp, probe.n_cgp_fail, probe.n_draw, probe.n_drawi, probe.n_brp);
}

// ============================================================================ interception
// vkGetDeviceQueue: the ONLY reason to intercept it is to cache the single queue that acquire has
// to submit its semaphore signal on.
static PFN_vkGetDeviceQueue h_getqueue;
static VKAPI_ATTR void VKAPI_CALL
c1_GetDeviceQueue(VkDevice d, uint32_t fam, uint32_t idx, VkQueue* q) {
  h_getqueue(d, fam, idx, q);
  if (q && *q && !g_queue) { g_queue = *q; LOG("cached VkQueue %p (family %u index %u)",
                                              (void*)*q, fam, idx); }
}

// vkCreateDevice: remember which physical device this VkDevice came from, because the swapchain
// needs vkGetPhysicalDeviceMemoryProperties and the WSI entry points are only handed a VkDevice.
//
// P3 also ENABLES VK_EXT_external_memory_host here. zink has no reason to ask for it — the
// extension is for this file's benefit, not the client's — and an extension that is not enabled
// at vkCreateDevice cannot be used later. Adding one the caller did not request is safe (it grants
// capability, it does not change behaviour) and it is done only when the physical device actually
// reports it, so a MoltenVK without it simply leaves the staging path in place.
static PFN_vkCreateDevice h_createdev;
static int phys_has_ext(VkPhysicalDevice pd, const char* want) {
  if (!g_gipa || !g_instance) return 0;
  PFN_vkEnumerateDeviceExtensionProperties e =
    (PFN_vkEnumerateDeviceExtensionProperties)g_gipa(g_instance, "vkEnumerateDeviceExtensionProperties");
  if (!e) return 0;
  uint32_t n = 0;
  if (e(pd, NULL, &n, NULL) != VK_SUCCESS || !n) return 0;
  VkExtensionProperties* p = calloc(n, sizeof *p);
  if (!p) return 0;
  int found = 0;
  if (e(pd, NULL, &n, p) >= 0)
    for (uint32_t i = 0; i < n; i++) if (!strcmp(p[i].extensionName, want)) { found = 1; break; }
  free(p);
  return found;
}

static VKAPI_ATTR VkResult VKAPI_CALL
c1_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci,
                const VkAllocationCallbacks* a, VkDevice* out) {
  g_phys = pd;
  if (!g_noimport && ci && phys_has_ext(pd, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) {
    uint32_t n = ci->enabledExtensionCount;
    int already = 0;
    for (uint32_t i = 0; i < n; i++)
      if (!strcmp(ci->ppEnabledExtensionNames[i], VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME))
        already = 1;
    const char** names = NULL;
    VkDeviceCreateInfo copy = *ci;
    if (!already && (names = calloc(n + 1, sizeof *names))) {
      for (uint32_t i = 0; i < n; i++) names[i] = ci->ppEnabledExtensionNames[i];
      names[n] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
      copy.enabledExtensionCount = n + 1;
      copy.ppEnabledExtensionNames = names;
      ci = &copy;
    }
    if (already || names) {
      g_ext_mem_host = 1;
      // minImportedHostPointerAlignment decides both whether the wl_shm mapping can be imported
      // at all and how far up the pool size has to be rounded.
      PFN_vkGetPhysicalDeviceProperties2 gp2 =
        (PFN_vkGetPhysicalDeviceProperties2)g_gipa(g_instance, "vkGetPhysicalDeviceProperties2");
      if (!gp2) gp2 = (PFN_vkGetPhysicalDeviceProperties2)
                      g_gipa(g_instance, "vkGetPhysicalDeviceProperties2KHR");
      if (gp2) {
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp = {
          .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT };
        VkPhysicalDeviceProperties2 p2 = {
          .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &hp };
        gp2(pd, &p2);
        g_import_align = (size_t)hp.minImportedHostPointerAlignment;
        LOG("VK_EXT_external_memory_host enabled; minImportedHostPointerAlignment=%zu",
            g_import_align);
      }
    }
    VkResult r = h_createdev(pd, ci, a, out);
    free(names);
    if (r != VK_SUCCESS) { g_ext_mem_host = 0; g_import_align = 0; }
    return r;
  }
  return h_createdev(pd, ci, a, out);
}

// vkCreateInstance: strip the extensions this file provides before MoltenVK sees them, or it
// rejects the whole instance with VK_ERROR_EXTENSION_NOT_PRESENT for an extension IT does not have
// and WE do. Also remembers the instance handle for physical-device-level procs.
static PFN_vkCreateInstance h_createinst;
static PFN_vkEnumerateInstanceExtensionProperties h_enuminstext;

static VKAPI_ATTR VkResult VKAPI_CALL
c1_CreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* a, VkInstance* out) {
  // (h_enuminstext is set when the host's enumerator is wrapped, below.)
  VkInstanceCreateInfo copy;
  const char* names[64];
  if (ci && ci->enabledExtensionCount && ci->enabledExtensionCount <= 64) {
    uint32_t n = 0, dropped = 0;
    // WHAT DOES THE HOST DRIVER ACTUALLY HAVE? The old code dropped one hardcoded name and
    // forwarded the rest, which worked only because MoltenVK does not validate the instance
    // extension list. Mesa's KosmicKrisp does, and returns VK_ERROR_EXTENSION_NOT_PRESENT (-7)
    // for anything it does not implement -- which the loader then reports to the application as
    // the far less helpful VK_ERROR_INCOMPATIBLE_DRIVER. So ask the host what it supports and
    // forward only that, which makes this ICD driver-agnostic instead of MoltenVK-shaped.
    VkExtensionProperties hostext[256];
    uint32_t nhost = 0;
    if (h_enuminstext) {
      uint32_t q = 0;
      if (h_enuminstext(NULL, &q, NULL) == VK_SUCCESS && q) {
        nhost = q > 256 ? 256 : q;
        if (h_enuminstext(NULL, &nhost, hostext) < 0) nhost = 0;
      }
    }
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
      const char* e = ci->ppEnabledExtensionNames[i];
      // Ours: implemented in this file, so the host must never be asked for them.
      if (!strcmp(e, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME) ||
          !strcmp(e, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME)) { dropped++; continue; }
      if (nhost) {                                   // theirs: only if they really have it
        int have = 0;
        for (uint32_t k = 0; k < nhost && !have; k++)
          if (!strcmp(e, hostext[k].extensionName)) have = 1;
        if (!have) {
          WARN("vkCreateInstance: host driver does not implement %s — dropping it rather than "
               "letting the whole instance fail", e);
          dropped++; continue;
        }
      }
      names[n++] = e;
    }
    if (dropped) {
      copy = *ci;
      copy.enabledExtensionCount = n;
      copy.ppEnabledExtensionNames = names;
      LOG("vkCreateInstance: hid %u extension(s) this ICD implements from MoltenVK", dropped);
      VkResult r = h_createinst(&copy, a, out);
      if (r == VK_SUCCESS && out) g_instance = *out;
      return r;
    }
  }
  VkResult r = h_createinst(ci, a, out);
  if (r == VK_SUCCESS && out) g_instance = *out;
  return r;
}

// vkEnumerateInstanceExtensionProperties: advertise VK_KHR_wayland_surface on top of MoltenVK's
// own list. The Vulkan loader only offers a surface extension to the application if some ICD
// claims it, so without this line zink never even asks — which is exactly the NULL slot it then
// calls. (VK_KHR_surface is already MoltenVK's, so it is not added twice.)
static VKAPI_ATTR VkResult VKAPI_CALL
c1_EnumerateInstanceExtensionProperties(const char* layer, uint32_t* n, VkExtensionProperties* p) {
  if (!n) return VK_ERROR_INITIALIZATION_FAILED;
  uint32_t hostn = 0;
  VkResult r = h_enuminstext(layer, &hostn, NULL);
  if (r != VK_SUCCESS) return r;
  static const struct { const char* name; uint32_t ver; } kExtra[] = {
    { VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME, 6 },
    { VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME, 1 },
  };
  const uint32_t nextra = (uint32_t)(sizeof kExtra / sizeof kExtra[0]);
  if (!p) { *n = hostn + nextra; return VK_SUCCESS; }
  uint32_t cap = *n;
  uint32_t take = cap < hostn ? cap : hostn;
  if (take) { uint32_t t = take; r = h_enuminstext(layer, &t, p); if (r < 0) return r; }
  uint32_t k = take;
  for (uint32_t i = 0; i < nextra && k < cap; i++, k++) {
    memset(&p[k], 0, sizeof p[k]);
    snprintf(p[k].extensionName, sizeof p[k].extensionName, "%s", kExtra[i].name);
    p[k].specVersion = kExtra[i].ver;
  }
  *n = k;
  return k == hostn + nextra ? VK_SUCCESS : VK_INCOMPLETE;
}

struct entry { const char* name; void* fn; };
static const struct entry kInstance[] = {
  { "vkCreateWaylandSurfaceKHR",                        (void*)c1_CreateWaylandSurfaceKHR },
  { "vkCreateHeadlessSurfaceEXT",                       (void*)c1_CreateHeadlessSurfaceEXT },
  { "vkDestroySurfaceKHR",                              (void*)c1_DestroySurfaceKHR },
  { "vkGetPhysicalDeviceSurfaceSupportKHR",             (void*)c1_GetPhysicalDeviceSurfaceSupportKHR },
  { "vkGetPhysicalDeviceSurfaceCapabilitiesKHR",        (void*)c1_GetPhysicalDeviceSurfaceCapabilitiesKHR },
  { "vkGetPhysicalDeviceSurfaceCapabilities2KHR",       (void*)c1_GetPhysicalDeviceSurfaceCapabilities2KHR },
  { "vkGetPhysicalDeviceSurfaceFormatsKHR",             (void*)c1_GetPhysicalDeviceSurfaceFormatsKHR },
  { "vkGetPhysicalDeviceSurfaceFormats2KHR",            (void*)c1_GetPhysicalDeviceSurfaceFormats2KHR },
  { "vkGetPhysicalDeviceSurfacePresentModesKHR",        (void*)c1_GetPhysicalDeviceSurfacePresentModesKHR },
  { "vkGetPhysicalDeviceWaylandPresentationSupportKHR", (void*)c1_GetPhysicalDeviceWaylandPresentationSupportKHR },
};
static const struct entry kDevice[] = {
  { "vkCreateSwapchainKHR",   (void*)c1_CreateSwapchainKHR },
  { "vkDestroySwapchainKHR",  (void*)c1_DestroySwapchainKHR },
  { "vkGetSwapchainImagesKHR",(void*)c1_GetSwapchainImagesKHR },
  { "vkAcquireNextImageKHR",  (void*)c1_AcquireNextImageKHR },
  { "vkAcquireNextImage2KHR", (void*)c1_AcquireNextImage2KHR },
  { "vkQueuePresentKHR",      (void*)c1_QueuePresentKHR },
};

void c1wsi_init(PFN_gipa_t gipa) {
  static int done;
  if (done) return;
  done = 1;
  g_gipa = gipa;
  g_v = getenv("C1WSI_VERBOSE") != 0;
  g_trace = getenv("C1WSI_TRACE") != 0;
  if (g_trace) atexit(c1wsi_report);
  g_stats = getenv("C1WSI_STATS") != 0;
  g_noimport = getenv("C1WSI_NOIMPORT") != 0;
  // DEFERRAL IS OPT-IN, and that is a measured decision rather than caution.
  // min-of-5 of the final cumulative frame period, one binary, one variable, replicated under both
  // RDSIGTRAP settings (so twice, independently):
  //     300x300   blocking 0.629 / 0.572 ms   deferred 0.527 / 0.485 ms   -> 16% / 15% FASTER
  //   1024x1024   blocking 1.177 / 1.154 ms   deferred 1.216 / 1.165 ms   ->  3% / 1% SLOWER
  // The legs say why: at 1024 wait-for-release grows by ~0.22 ms, almost exactly the fence wait
  // the deferral removed. The time does not vanish, it moves — because at that size the frame
  // period is set by the COMPOSITOR draining the pool, not by our round trip, and running a frame
  // further ahead just relocates the wait and costs a frame of latency for it. A fourth image
  // recovers part but not all of it (1.202 -> 1.157 vs 1.120 blocking), so it is not purely slack.
  // Turning this on by default would have shipped a regression at every ordinary window size on
  // the strength of a win at the one size that was benchmarked first.
  g_nodefer  = getenv("C1WSI_DEFER") == 0 || getenv("C1WSI_NODEFER") != 0;
  LOG("WSI layer active: VK_KHR_wayland_surface + VK_KHR_swapchain are provided by the ICD");
}

// Called from b7icd.c's vk_icdGetInstanceProcAddr BEFORE forwarding. Returns our implementation,
// or NULL to mean "not ours, forward to MoltenVK as usual".
void* c1wsi_instance_proc(void* instance, const char* name, void* host) {
  if (!name) return NULL;
  if (instance) g_instance = (VkInstance)instance;
  for (unsigned i = 0; i < sizeof kInstance / sizeof kInstance[0]; i++)
    if (!strcmp(name, kInstance[i].name)) return kInstance[i].fn;
  // These three wrap the host's own implementation rather than replacing it.
  if (!strcmp(name, "vkEnumerateInstanceExtensionProperties") && host) {
    h_enuminstext = (PFN_vkEnumerateInstanceExtensionProperties)host;
    return (void*)c1_EnumerateInstanceExtensionProperties;
  }
  if (!strcmp(name, "vkCreateInstance") && host) {
    h_createinst = (PFN_vkCreateInstance)host;
    return (void*)c1_CreateInstance;
  }
  if (!strcmp(name, "vkCreateDevice") && host) {
    h_createdev = (PFN_vkCreateDevice)host;
    return (void*)c1_CreateDevice;
  }
  return NULL;
}

// Called from b7icd.c's vkGetDeviceProcAddr trampoline.
void* c1wsi_device_proc(void* dev, const char* name, void* host, void* gdpa) {
  (void)dev;
  if (!name) return NULL;
  if (gdpa && !vk.gdpa) vk.gdpa = (PFN_vkGetDeviceProcAddr)gdpa;
  for (unsigned i = 0; i < sizeof kDevice / sizeof kDevice[0]; i++)
    if (!strcmp(name, kDevice[i].name)) return kDevice[i].fn;
  if (!strcmp(name, "vkGetDeviceQueue") && host) {
    h_getqueue = (PFN_vkGetDeviceQueue)host;
    return (void*)c1_GetDeviceQueue;
  }
  if (g_trace && host) {
    if (!strcmp(name, "vkCreateGraphicsPipelines")) { probe.cgp = (PFN_vkCreateGraphicsPipelines)host; return (void*)p_CreateGraphicsPipelines; }
    if (!strcmp(name, "vkCmdDraw"))                 { probe.draw = (PFN_vkCmdDraw)host;                return (void*)p_CmdDraw; }
    if (!strcmp(name, "vkCmdDrawIndexed"))          { probe.drawi = (PFN_vkCmdDrawIndexed)host;        return (void*)p_CmdDrawIndexed; }
    if (!strcmp(name, "vkCmdBeginRenderPass"))      { probe.brp = (PFN_vkCmdBeginRenderPass)host;      return (void*)p_CmdBeginRenderPass; }
  }
  return NULL;
}
