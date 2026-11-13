// b7icd.c — THE MISSING LINK: a guest-side Vulkan ICD that forwards to host MoltenVK.
//
// B5's audit found that every other piece of the chain
//     app -> guest Mesa/zink (OpenGL-on-Vulkan) -> Vulkan -> host MoltenVK -> Metal -> GPU
// is already installed on this machine: zink is compiled into the guest's libgallium_dri.so
// (Mesa 24.0.9, exports __driDriverGetExtensions_zink), the guest has a Vulkan loader
// (libvulkan.so.1 1.3.261) that honours VK_ICD_FILENAMES / VK_DRIVER_FILES, and MoltenVK 1.4.0 is
// on the host. Exactly ONE component was missing: this file.
//
// WHY IT IS THIS SHORT. B5 sized it at ~400 generated trampolines plus a shim round trip on every
// Vulkan call, because the shim has no way to call host code from the guest except syscall 0x2001.
// tests/wl/b7direct.c measured that assumption and it does not hold on this tree: guest code can
// call a host MoltenVK function pointer DIRECTLY, at 11.2 ns/call against 12624 ns through the
// shim, and tests/wl/b7gpu.c --both builds and renders an entire Vulkan pipeline that way,
// including the Metal shader compile, producing byte-identical pixels to the shim-mediated route.
//
// So this ICD does not marshal, wrap, or trampoline anything. It resolves MoltenVK's own
// vk_icdGetInstanceProcAddr once and hands the guest loader HOST function pointers, which the
// loader and zink then call as ordinary indirect calls. Every Vulkan call in a frame is a plain
// `blr` with the shim uninvolved. Two reasons that is safe rather than merely convenient:
//
//   * ONE ADDRESS SPACE. gpu.h: "guest pointers are real host addresses". A Vulkan struct built by
//     guest zink is directly readable by host MoltenVK and a vkMapMemory pointer is directly
//     writable by the guest, with no copy — this is B5's measured zero-copy result.
//   * THE ABIs AGREE. Linux and Darwin aarch64 are both AAPCS64, and they differ only on variadics
//     and small-struct-by-value, neither of which Vulkan uses.
//
// WHAT THIS DELIBERATELY DOES NOT DO, so it is not mistaken for more than it is: it provides no
// WSI. MoltenVK has VK_EXT_metal_surface and not VK_KHR_wayland_surface, so a guest Wayland
// surface cannot be bound as a swapchain and zink's kopper path has nothing to attach to. The v1
// answer is to render offscreen and read back through the existing wl_shm present path, which the
// measured cost licenses (readback 0.67 ms at 1080p against the ~85 ms of llvmpipe it replaces).
//
// Build:  tools/build-b7icd.sh      Use:  VK_ICD_FILENAMES=/b7icd.json  (or VK_DRIVER_FILES)

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

extern long syscall(long, ...);

#define HOSTCALL_SYSCALL 0x2001
enum { HC_DLOPEN = 1, HC_DLSYM = 2 };
struct hostcall_req { uint32_t op; uint32_t nargs; uint64_t a[8]; uint64_t result; };

static int hc(struct hostcall_req* r) { return (int)syscall(HOSTCALL_SYSCALL, r); }

static uint64_t host_dlopen(const char* p) {
  struct hostcall_req r; memset(&r, 0, sizeof r);
  r.op = HC_DLOPEN; r.a[0] = (uint64_t)(uintptr_t)p;
  return hc(&r) < 0 ? 0 : r.result;
}
static uint64_t host_dlsym(uint64_t h, const char* n) {
  struct hostcall_req r; memset(&r, 0, sizeof r);
  r.op = HC_DLSYM; r.a[0] = h; r.a[1] = (uint64_t)(uintptr_t)n;
  return hc(&r) < 0 ? 0 : r.result;
}

typedef void* (*PFN_gipa)(void* instance, const char* name);

static uint64_t     g_mvk;
static PFN_gipa     g_icd_gipa;      // MoltenVK's vk_icdGetInstanceProcAddr
static PFN_gipa     g_icd_gpdpa;     // MoltenVK's vk_icdGetPhysicalDeviceProcAddr (may be absent)
static int          g_verbose;
static int          g_trace;
static int          g_fixup = 1;      // on by default: it only engages after a real failure
static int          g_fake;           // B7ICD_FAKEFEATS: claim zink's base requirements
static int          g_fixup_said;

static int b7icd_up(void) {
  if (g_icd_gipa) return 1;
  const char* path = getenv("B7ICD_MOLTENVK");
  if (!path || !*path) path = "/opt/homebrew/lib/libMoltenVK.dylib";
  g_verbose = getenv("B7ICD_VERBOSE") != 0;
  g_trace   = getenv("B7ICD_TRACE")   != 0;
  { const char* f = getenv("B7ICD_FIXUP"); if (f && *f == '0') g_fixup = 0; }
  { const char* f = getenv("B7ICD_FAKEFEATS"); g_fake = f && *f ? atoi(f) : 0; }

  g_mvk = host_dlopen(path);
  if (!g_mvk) {
    fprintf(stderr, "[b7icd] cannot dlopen host %s — is RDHOSTCALL=1 set?\n", path);
    return 0;
  }
  // MoltenVK is itself an ICD on macOS, so it exports the ICD entry points; forward to them
  // rather than to the plain vkGetInstanceProcAddr, so the loader's negotiated ICD semantics
  // (physical-device procs, unknown-extension handling) are MoltenVK's own and not reinvented.
  g_icd_gipa  = (PFN_gipa)(uintptr_t)host_dlsym(g_mvk, "vk_icdGetInstanceProcAddr");
  g_icd_gpdpa = (PFN_gipa)(uintptr_t)host_dlsym(g_mvk, "vk_icdGetPhysicalDeviceProcAddr");
  if (!g_icd_gipa)  // older MoltenVK: fall back to the core entry point
    g_icd_gipa = (PFN_gipa)(uintptr_t)host_dlsym(g_mvk, "vkGetInstanceProcAddr");
  if (!g_icd_gipa) { fprintf(stderr, "[b7icd] host MoltenVK has no vkGetInstanceProcAddr\n"); return 0; }
  if (g_verbose) fprintf(stderr, "[b7icd] host MoltenVK up: gipa=%p gpdpa=%p\n",
                         (void*)g_icd_gipa, (void*)g_icd_gpdpa);
  return 1;
}

// ---------------------------------------------------------------------------------------------
// OPTIONAL TRACE LAYER (B7ICD_TRACE=1). Off by default and returning host pointers unchanged, so
// it costs nothing in the normal path.
//
// It exists because the interesting failures in this lane are of the form "Mesa asked for
// something MoltenVK will not give it", and from outside that is indistinguishable from "the ICD
// is broken". Wrapping a handful of entry points on the guest side turns a crash into a specific
// request and a specific VkResult. This is the trampoline shape B5 expected to need for ALL ~400
// entry points; it is used here for four, purely as an instrument.
// ---------------------------------------------------------------------------------------------

// ---------------------------------------------------------------------------------------------
// C1: the WSI layer (src/c1wsi.c), compiled in ONLY with -DC1_WSI so that the shipping b7icd.so
// stays byte-for-byte what B7 measured. Without it this ICD is unchanged; with it the ICD also
// provides VK_KHR_wayland_surface / VK_KHR_swapchain, which is what a WINDOWED client needs.
// ---------------------------------------------------------------------------------------------
#ifdef C1_WSI
void  c1wsi_init(void* (*gipa)(void*, const char*));
void* c1wsi_instance_proc(void* instance, const char* name, void* host);
void* c1wsi_device_proc(void* dev, const char* name, void* host, void* gdpa);
#endif

typedef int32_t (*PFN_alloc)(void* dev, const void* ai, const void* alloc, uint64_t* mem);
typedef void*   (*PFN_gdpa)(void* dev, const char* name);
static PFN_alloc h_alloc;
static PFN_gdpa  h_gdpa;

// VkMemoryAllocateInfo: sType(0) pad(4) pNext(8) allocationSize(16) memoryTypeIndex(24)
// Every struct in a pNext chain starts with { sType, pad, pNext }, so the chain can be walked
// without knowing any of the member types.
static int32_t t_vkAllocateMemory(void* dev, const void* ai, const void* alloc, uint64_t* mem) {
  const uint8_t* p = (const uint8_t*)ai;
  uint64_t size = 0; uint32_t mti = 0;
  memcpy(&size, p + 16, 8); memcpy(&mti, p + 24, 4);
  char chain[320]; chain[0] = 0;
  const uint8_t* nx; memcpy(&nx, p + 8, 8);
  for (int i = 0; nx && i < 6; i++) {
    uint32_t st; memcpy(&st, nx, 4);
    char b[96];
    if (st == 1000060000) {            // VkMemoryAllocateFlagsInfo: flags(16) deviceMask(20)
      uint32_t fl, dm; memcpy(&fl, nx + 16, 4); memcpy(&dm, nx + 20, 4);
      snprintf(b, sizeof b, " [AllocateFlags flags=%#x deviceMask=%#x]", fl, dm);
    } else if (st == 1000127001) {     // VkMemoryDedicatedAllocateInfo: image(16) buffer(24)
      uint64_t im, bu; memcpy(&im, nx + 16, 8); memcpy(&bu, nx + 24, 8);
      snprintf(b, sizeof b, " [Dedicated image=%#llx buffer=%#llx]",
               (unsigned long long)im, (unsigned long long)bu);
    } else snprintf(b, sizeof b, " [sType=%u]", st);
    if (strlen(chain) + strlen(b) < sizeof chain - 1) strcat(chain, b);
    const uint8_t* n2; memcpy(&n2, nx + 8, 8); nx = n2;
  }
  int32_t r = h_alloc(dev, ai, alloc, mem);
  if (g_trace)
    fprintf(stderr, "[b7icd] vkAllocateMemory size=%llu (%.1f MiB) memoryTypeIndex=%u%s -> %d%s\n",
            (unsigned long long)size, (double)size / (1024.0 * 1024.0), mti,
            chain[0] ? chain : " (no pNext)", r, r ? "  <<< FAILED" : "");
  if (r == 0 || !g_fixup) return r;

  // ---- MOLTENVK COMPATIBILITY FIXUP (see the block comment above b7icd_fixup_note) ----------
  // Retry without the dedicated-allocation request. Engaged ONLY after a real failure, so it can
  // never mask correct behaviour, and it is a no-op unless a dedicated-image info is present.
  const uint8_t* d = 0; const uint8_t* prev = 0; const uint8_t* n = 0;
  memcpy(&n, p + 8, 8);
  for (int i = 0; n && i < 8; i++) {
    uint32_t st; memcpy(&st, n, 4);
    if (st == 1000127001) { uint64_t im; memcpy(&im, n + 16, 8); if (im) { d = n; break; } }
    prev = n; const uint8_t* n2; memcpy(&n2, n + 8, 8); n = n2;
  }
  if (!d) return r;

  uint8_t ai2[64]; memcpy(ai2, p, 32);          // VkMemoryAllocateInfo is 32 bytes
  const uint8_t* after; memcpy(&after, d + 8, 8);
  if (prev) {
    // Unlink d from a COPY of the intermediate struct, so the caller's memory is never mutated.
    static uint8_t mid[128];
    memcpy(mid, prev, sizeof mid > 64 ? 64 : sizeof mid);
    memcpy(mid + 8, &after, 8);
    const uint8_t* head; memcpy(&head, p + 8, 8);
    if (head == prev) { const uint8_t* mp = mid; memcpy(ai2 + 8, &mp, 8); }
    else return r;                              // deeper chain than this fixup understands
  } else {
    memcpy(ai2 + 8, &after, 8);                 // the dedicated info was the chain head
  }
  int32_t r2 = h_alloc(dev, ai2, alloc, mem);
  if (!g_fixup_said) {
    g_fixup_said = 1;
    fprintf(stderr, "[b7icd] NOTE: MoltenVK refused a DEDICATED image allocation on a memory type "
            "its own vkGetImageMemoryRequirements reported as allowed; retried without the "
            "dedicated request -> %d. Set B7ICD_FIXUP=0 to see the unpatched behaviour.\n", r2);
  }
  if (g_trace) fprintf(stderr, "[b7icd]   fixup retry -> %d\n", r2);
  return r2;
}


// ---------------------------------------------------------------------------------------------
// B7ICD_FAKEFEATS=1 — claim the three features zink treats as BASE REQUIREMENTS.
//
// MEASURED (tests/wl/eglcore.c, surfaceless, no window system): zink on this device gives
// OpenGL 2.1 and eglCreateContext for 3.3 CORE fails with EGL_BAD_MATCH, while llvmpipe gives 4.5
// core through the identical probe. Blender needs 3.3 core, so the GPU is unusable to it. zink
// names its reason at startup: feats.features.logicOp, VK_EXT_custom_border_color and
// VK_EXT_depth_clip_enable are missing.
//
// THIS IS KNOWINGLY CLAIMING SOMETHING METAL CANNOT DO. Metal has no fixed-function logic ops, so
// MoltenVK cannot offer logicOp honestly and neither can we. The bet is narrow and testable: that
// zink GATES on the feature bits rather than requiring them for the paths a GL 3.3 core app takes
// (logic ops are legacy fixed-function state that modern GL content does not use). If content
// renders correctly the bits were a gate; if it corrupts, this reverts. It is OFF by default for
// exactly that reason.
//
// The lie is told to the CLIENT only. vkCreateDevice strips the same extensions and clears the
// same bit before the call reaches MoltenVK, which would otherwise reject the device outright.
// ---------------------------------------------------------------------------------------------
static int g_fake_said;

#define ST_CUSTOM_BORDER_COLOR_FEATS 1000287004
#define ST_DEPTH_CLIP_ENABLE_FEATS   1000102000
static const char* const kFakeExt[] = { "VK_EXT_custom_border_color", "VK_EXT_depth_clip_enable" };

typedef void    (*PFN_pdf)(void*, void*);
typedef void    (*PFN_pdf2)(void*, void*);
typedef int32_t (*PFN_edep)(void*, const char*, uint32_t*, void*);
typedef int32_t (*PFN_cdev)(void*, const void*, const void*, void*);
static PFN_pdf  h_pdf;
static PFN_pdf2 h_pdf2;
static PFN_edep h_edep;
static PFN_cdev h_cdev;

// VkPhysicalDeviceRobustness2FeaturesEXT: sType(0) pNext(8) robustBufferAccess2(16)
// robustImageAccess2(20) nullDescriptor(24). MoltenVK SHIPS this extension and reports
// nullDescriptor=FALSE, and Mesa 26's zink refuses to start without it -- it is the FIRST
// blocker on the GL path, ahead of transform feedback.
#define ST_ROBUSTNESS2_FEATS 1000286000
#define ROB2_NULLDESC_OFF    24

// VkPhysicalDeviceFeatures.logicOp is the 9th VkBool32 -> byte 32.
#define FEAT_LOGICOP_OFF 32

static void fake_say(void) {
  if (g_fake_said) return;
  g_fake_said = 1;
  fprintf(stderr, "[b7icd] B7ICD_FAKEFEATS: reporting logicOp + custom_border_color + "
                  "depth_clip_enable + robustness2.nullDescriptor as SUPPORTED to the client, "
                  "and stripping them from vkCreateDevice. Metal has no logic ops and MoltenVK "
                  "does not implement nullDescriptor -- if rendering is wrong, this is why.\n");
}

static void t_vkGetPhysicalDeviceFeatures(void* pd, void* pf) {
  h_pdf(pd, pf);
  if (!g_fake || !pf) return;
  uint32_t t = 1; memcpy((uint8_t*)pf + FEAT_LOGICOP_OFF, &t, 4);
  fake_say();
}

static void t_vkGetPhysicalDeviceFeatures2(void* pd, void* pf2) {
  h_pdf2(pd, pf2);
  if (!g_fake || !pf2) return;
  uint32_t t = 1;
  memcpy((uint8_t*)pf2 + 16 + FEAT_LOGICOP_OFF, &t, 4);   // .features.logicOp
  uint8_t* nx; memcpy(&nx, (uint8_t*)pf2 + 8, 8);
  for (int i = 0; nx && i < 32; i++) {
    uint32_t st; memcpy(&st, nx, 4);
    if (st == ST_CUSTOM_BORDER_COLOR_FEATS) {
      memcpy(nx + 16, &t, 4); memcpy(nx + 20, &t, 4);    // customBorderColors[, WithoutFormat]
    } else if (st == ST_DEPTH_CLIP_ENABLE_FEATS) {
      memcpy(nx + 16, &t, 4);                            // depthClipEnable
    } else if (st == ST_ROBUSTNESS2_FEATS) {
      memcpy(nx + ROB2_NULLDESC_OFF, &t, 4);             // nullDescriptor
    }
    uint8_t* n2; memcpy(&n2, nx + 8, 8); nx = n2;
  }
  fake_say();
}

// VkExtensionProperties = char[256] + uint32 = 260 bytes.
#define VK_EXT_PROP_SZ 260
static int32_t t_vkEnumerateDeviceExtensionProperties(void* pd, const char* layer,
                                                      uint32_t* count, void* props) {
  // FAKEFEATS=1 fakes the FEATURE BITS ONLY. Injecting extensions as well (=2) is what makes zink
  // enable them, and they cannot be stripped again: vkCreateDevice is never queried through our
  // GIPA and exporting the symbol does not help either, because this ICD hands the loader HOST
  // function pointers -- once MoltenVK's real vkCreateDevice is resolved, the call bypasses us
  // entirely. So =2 ends in VK_ERROR_EXTENSION_NOT_PRESENT by construction.
  if (g_fake < 2) return h_edep(pd, layer, count, props);
  uint32_t nfake = (uint32_t)(sizeof kFakeExt / sizeof kFakeExt[0]);
  if (!props) {                                   // size query: add room for ours
    int32_t r = h_edep(pd, layer, count, 0);
    if (r >= 0 && count) *count += nfake;
    return r;
  }
  uint32_t have = *count;
  if (have < nfake) return h_edep(pd, layer, count, props);
  uint32_t room = have - nfake;
  int32_t r = h_edep(pd, layer, &room, props);
  if (r < 0) return r;
  for (uint32_t i = 0; i < nfake; i++) {          // append ours after what MoltenVK reported
    uint8_t* e = (uint8_t*)props + (size_t)(room + i) * VK_EXT_PROP_SZ;
    memset(e, 0, VK_EXT_PROP_SZ);
    strncpy((char*)e, kFakeExt[i], 255);
    uint32_t ver = 1; memcpy(e + 256, &ver, 4);
  }
  *count = room + nfake;
  return r;
}

// Strip what we invented before MoltenVK sees it: it would reject the device for an extension it
// does not implement, and clearing the feature bit keeps the enabled set honest at the real driver.
static int32_t t_vkCreateDevice(void* pd, const void* ci, const void* alloc, void* dev) {
  if (!g_fake) return h_cdev(pd, ci, alloc, dev);
  uint8_t copy[128];
  size_t cisz = 72;                                // through pEnabledFeatures
  if (cisz > sizeof copy) return h_cdev(pd, ci, alloc, dev);
  memcpy(copy, ci, cisz);
  uint32_t next; memcpy(&next, copy + 48, 4);
  const char** names; memcpy(&names, copy + 56, 8);
  const char* keep[64];
  uint32_t nk = 0;
  for (uint32_t i = 0; i < next && nk < 64; i++) {
    int drop = 0;
    for (uint32_t j = 0; j < sizeof kFakeExt / sizeof kFakeExt[0]; j++)
      if (names[i] && !strcmp(names[i], kFakeExt[j])) drop = 1;
    if (!drop) keep[nk++] = names[i];
  }
  memcpy(copy + 48, &nk, 4);
  const char** kp = keep; memcpy(copy + 56, &kp, 8);
  // pEnabledFeatures: clear logicOp in a private copy if one was supplied.
  uint8_t* pef; memcpy(&pef, copy + 64, 8);
  uint8_t featcopy[256];
  if (pef) {
    memcpy(featcopy, pef, sizeof featcopy);
    uint32_t z = 0; memcpy(featcopy + FEAT_LOGICOP_OFF, &z, 4);
    uint8_t* fp = featcopy; memcpy(copy + 64, &fp, 8);
  }
  // And in any VkPhysicalDeviceFeatures2 on the pNext chain.
  uint8_t* nx; memcpy(&nx, copy + 8, 8);
  for (int i = 0; nx && i < 32; i++) {
    uint32_t st; memcpy(&st, nx, 4);
    if (st == 1000059000) {                        // PHYSICAL_DEVICE_FEATURES_2
      uint32_t z = 0; memcpy(nx + 16 + FEAT_LOGICOP_OFF, &z, 4);
    } else if (st == ST_ROBUSTNESS2_FEATS) {
      uint32_t z = 0; memcpy(nx + ROB2_NULLDESC_OFF, &z, 4);   // never ask MoltenVK for it
    }
    uint8_t* n2; memcpy(&n2, nx + 8, 8); nx = n2;
  }
  int32_t r = h_cdev(pd, copy, alloc, dev);
  if (g_trace) fprintf(stderr, "[b7icd] vkCreateDevice (stripped %u ext) -> %d\n", next - nk, r);
  return r;
}

// Instance- and physical-device-level hooks share this.
static void* fake_hook(const char* name, void* p) {
  // Trace BEFORE the null check: a name the host resolved to NULL is exactly the case that would
  // otherwise be invisible, and "was it never asked for, or asked for and unavailable?" is the
  // whole question when a hook does not fire.
  if (g_trace) fprintf(stderr, "[b7icd] fake_hook sees %s (p=%p)\n", name ? name : "(null)", p);
  if (!p || !name) return p;
  if (!strcmp(name, "vkGetPhysicalDeviceFeatures"))  { h_pdf  = (PFN_pdf)p;  return (void*)t_vkGetPhysicalDeviceFeatures; }
  if (!strcmp(name, "vkGetPhysicalDeviceFeatures2") ||
      !strcmp(name, "vkGetPhysicalDeviceFeatures2KHR")) { h_pdf2 = (PFN_pdf2)p; return (void*)t_vkGetPhysicalDeviceFeatures2; }
  if (!strcmp(name, "vkEnumerateDeviceExtensionProperties")) { h_edep = (PFN_edep)p; return (void*)t_vkEnumerateDeviceExtensionProperties; }
  if (!strcmp(name, "vkCreateDevice")) { h_cdev = (PFN_cdev)p; return (void*)t_vkCreateDevice; }
  return p;
}

static void* t_vkGetDeviceProcAddr(void* dev, const char* name) {
  void* p = h_gdpa(dev, name);
#ifdef C1_WSI
  { void* w = c1wsi_device_proc(dev, name, p, (void*)h_gdpa); if (w) return w; }
#endif
  // Device-level entry points are handed out here, not by the instance proc addr, so this is the
  // only place vkAllocateMemory can be intercepted.
  if (p && name && !strcmp(name, "vkAllocateMemory")) {
    h_alloc = (PFN_alloc)p; return (void*)t_vkAllocateMemory;
  }
  return p;
}

// ---- the three symbols the Vulkan loader looks for in an ICD -------------------------------
__attribute__((visibility("default")))
int32_t vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* pVersion) {
  if (!b7icd_up()) return -3;                       // VK_ERROR_INITIALIZATION_FAILED
  typedef int32_t (*PFN_neg)(uint32_t*);
  PFN_neg n = (PFN_neg)(uintptr_t)host_dlsym(g_mvk, "vk_icdNegotiateLoaderICDInterfaceVersion");
  if (n) return n(pVersion);
  // MoltenVK predating the negotiation call: report version 1, which the loader accepts and
  // which means "no physical-device-proc-addr, no interface extensions".
  if (pVersion && *pVersion > 1) *pVersion = 1;
  return 0;
}

__attribute__((visibility("default")))
void* vk_icdGetInstanceProcAddr(void* instance, const char* name) {
  if (!b7icd_up()) return 0;
#ifdef C1_WSI
  c1wsi_init((void* (*)(void*, const char*))g_icd_gipa);
#endif
  void* p = g_icd_gipa(instance, name);
#ifdef C1_WSI
  { void* w = c1wsi_instance_proc(instance, name, p); if (w) return w; }
#endif
  // The device-proc trampoline must be installed whenever EITHER the fixup/trace layer or the WSI
  // layer needs it. Without the C1_WSI clause the swapchain entry points are never handed out --
  // and the failure is silent, because the loader simply falls back to MoltenVK's own.
#ifdef C1_WSI
  if (p && name && !strcmp(name, "vkGetDeviceProcAddr")) { h_gdpa = (PFN_gdpa)p; return (void*)t_vkGetDeviceProcAddr; }
#endif
  if ((g_trace || g_fixup) && p && name && !strcmp(name, "vkGetDeviceProcAddr")) {
    h_gdpa = (PFN_gdpa)p; return (void*)t_vkGetDeviceProcAddr;
  }
  if (g_fake) { void* f = fake_hook(name, p); if (f != p) return f; }
  if (g_verbose && !p) fprintf(stderr, "[b7icd] miss: %s\n", name ? name : "(null)");
  return p;                                          // a HOST pointer, called directly by the guest
}

__attribute__((visibility("default")))
void* vk_icdGetPhysicalDeviceProcAddr(void* instance, const char* name) {
  if (!b7icd_up()) return 0;
  void* p = g_icd_gpdpa ? g_icd_gpdpa(instance, name) : 0;
  if (g_fake) { void* f = fake_hook(name, p); if (f != p) return f; }
  return p;
}

// EXPORTED vkCreateDevice. The loader never asks for this one through vk_icdGetInstanceProcAddr
// -- traced, with the probe placed before the null check so "asked for and unavailable" could not
// be confused with "never asked for": 42 entry points arrive and this is not among them. So the
// only way to intervene is to export the symbol and let the loader find it by name.
//
// It matters because B7ICD_FAKEFEATS invents two device extensions for zink's benefit, and if they
// reach MoltenVK unfiltered it rejects the device with VK_ERROR_EXTENSION_NOT_PRESENT -- which is
// exactly where the experiment stalled.
__attribute__((visibility("default")))
int32_t vkCreateDevice(void* pd, const void* ci, const void* alloc, void* dev) {
  if (!b7icd_up()) return -3;                        // VK_ERROR_INITIALIZATION_FAILED
  if (!h_cdev) {
    // Resolve MoltenVK's own directly: no instance is needed and it cannot depend on the loader
    // having handed us anything.
    h_cdev = (PFN_cdev)(uintptr_t)host_dlsym(g_mvk, "vkCreateDevice");
    if (!h_cdev && g_icd_gipa) h_cdev = (PFN_cdev)g_icd_gipa(0, "vkCreateDevice");
  }
  if (!h_cdev) { fprintf(stderr, "[b7icd] host MoltenVK has no vkCreateDevice\n"); return -3; }
  return t_vkCreateDevice(pd, ci, alloc, dev);
}

// Some loaders and some consumers (zink among them) also probe for the plain core name.
__attribute__((visibility("default")))
void* vkGetInstanceProcAddr(void* instance, const char* name) {
  return vk_icdGetInstanceProcAddr(instance, name);
}
