// hostcall.c — RESEARCH SPIKE (B5). Let guest Linux code call a HOST Mach-O function directly.
//
// WHY THIS EXISTS. The question in front of the project is whether "real GPU acceleration" is
// reachable, i.e. whether a modern-GL path can get to Metal. The credible route is
// zink (Mesa's OpenGL-on-Vulkan driver, already compiled into the guest's libgallium_dri.so)
// talking to MoltenVK (Vulkan-on-Metal, already installed on the host). The ONLY structural
// question in that chain is who carries Vulkan across the guest/host boundary. Under a VM you
// would have to invent a wire protocol and marshal every buffer. Here you do not, and this file
// is the proof of why:
//
//   * THE GUEST SHARES THE HOST ADDRESS SPACE. gpu.h already states it: "guest pointers are real
//     host addresses". A Vulkan struct built by guest zink is directly readable by host MoltenVK,
//     and a pointer from vkMapMemory is directly writable by the guest. Zero marshalling, zero
//     copies — the thing that makes GPU passthrough expensive under virtualisation is absent.
//   * THE CALLING CONVENTIONS ALREADY MATCH. Linux and Darwin on aarch64 both use AAPCS64. They
//     differ on variadics and on small-struct-in-registers rules; Vulkan uses neither — it is a
//     deliberately pointer-based ABI (every big struct is passed as `const VkXxx*`), which is
//     exactly the shape that survives the boundary untouched.
//   * THE REGISTER/STACK BRIDGE IS ALREADY BUILT AND SHIPPING. fastcall.h documents what a host
//     call needs — preserve the guest's x18, zero it for the host, and switch to a clean NATIVE
//     stack because macOS libsystem crashes on the guest's Linux stack. That is the fastcall
//     thunk, in production today at ~2 ns/call. And TLS needs NOTHING: macOS already owns
//     TPIDR_EL0 in this design and guest TLS lives elsewhere, so host code finds its thread
//     pointer already correct.
//
// So this is not a new mechanism. It is ~60 lines that expose the mechanism the shim already has
// to the guest as dlopen/dlsym/call, so the claim can be MEASURED instead of argued.
//
// SCOPE / SAFETY. Deliberately opt-in: every op returns -EPERM unless RDHOSTCALL=1 is set, so no
// existing gate, test or app can reach it and the default build behaves exactly as before. This
// is a spike to answer a design question, not a proposed interface — a real one would be a typed
// Vulkan ICD, not a general "call any host address".

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <dlfcn.h>

#define HC_EPERM  (-1)
#define HC_EINVAL (-22)
#define HC_EFAULT (-14)
#define HC_ENOSYS (-38)

// Guest-visible request block. Pointers in a[] are ordinary addresses: same address space.
struct hostcall_req {
  uint32_t op;
  uint32_t nargs;      // op HC_CALL: how many of a[1..] are real arguments (0..7)
  uint64_t a[8];
  uint64_t result;
};

// HC_CALLN is additive on purpose: the request block's layout is UNCHANGED, so B5's b5vk binary
// (which memsets its own copy of this struct) keeps working bit-for-bit. Widening a[] instead
// would have moved `result` and silently broken it.
//   HC_CALLN: a[0] = fn, a[1] = argc (0..16), a[2] = pointer to uint64_t argv[argc]
enum { HC_DLOPEN = 1, HC_DLSYM = 2, HC_CALL = 3, HC_CALLN = 4 };

// ARGUMENT ARITY (B7). B5 implemented 7 register arguments and left the stack-spill case
// deliberately unimplemented, naming it as blocker A. It is now implemented, because it is not
// optional for a renderer: vkGetQueryPoolResults takes 8 arguments and vkCmdPipelineBarrier takes
// 10, so a GPU frame cannot be issued without crossing the AAPCS64 register/stack boundary.
//
// HOW, and why it is done this way. AAPCS64 puts integer arguments 0..7 in x0..x7 and spills the
// rest onto the stack in order, and it is the CALLER that lays the stack out. So rather than write
// the spill by hand in assembly — which would have to be kept in step with the shim's own
// stack-switching thunk — this declares one function-pointer type per arity and lets the C
// compiler emit the correct AAPCS64 sequence for each. Calling a callee that declares FEWER
// parameters through a wider pointer type is safe here: the extra registers are ignored and the
// extra stack slots are the caller's own, popped by the caller.
//
// WHAT THIS STILL CANNOT DO, stated rather than discovered later: every argument is passed as an
// integer register/slot, so an entry point taking a FLOAT by value (vkCmdSetDepthBias, three
// floats in v0..v2; vkCmdSetLineWidth) is NOT callable through this channel. That is intrinsic to
// a generic blaster and is exactly why the shipping thing must be a typed, vk.xml-generated ICD
// with a real prototype per entry point, not this. No entry point on the render path takes a
// float by value — Vulkan passes them inside structs, by pointer.
#define A1  uint64_t
#define A2  A1,A1
#define A4  A2,A2
#define A8  A4,A4
typedef uint64_t (*hc_fn7)(A4,A2,A1);
typedef uint64_t (*hc_fn16)(A8,A8);
#define HC_MAXARGS 16

static int hostcall_enabled(void) {
  static int e = -1;
  if (e < 0) { const char* v = getenv("RDHOSTCALL"); e = (v && *v && *v != '0'); }
  return e;
}

long hostcall(struct hostcall_req* r) {
  if (!r) return HC_EFAULT;
  if (!hostcall_enabled()) return HC_EPERM;

  switch (r->op) {
  case HC_DLOPEN: {
    const char* path = (const char*)(uintptr_t)r->a[0];
    if (!path) return HC_EFAULT;
    void* h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "[hostcall] dlopen(%s) failed: %s\n", path, dlerror()); return HC_EINVAL; }
    r->result = (uint64_t)(uintptr_t)h;
    return 0;
  }
  case HC_DLSYM: {
    void* h = (void*)(uintptr_t)r->a[0];
    const char* name = (const char*)(uintptr_t)r->a[1];
    if (!name) return HC_EFAULT;
    void* p = dlsym(h, name);
    if (!p) { fprintf(stderr, "[hostcall] dlsym(%s) failed\n", name); return HC_EINVAL; }
    r->result = (uint64_t)(uintptr_t)p;
    return 0;
  }
  case HC_CALL: {
    hc_fn7 f = (hc_fn7)(uintptr_t)r->a[0];
    if (!f) return HC_EFAULT;
    if (r->nargs > 7) return HC_EINVAL;
    // Passing 7 always is safe under AAPCS64: unused argument registers are simply ignored by a
    // callee that declares fewer parameters.
    r->result = f(r->a[1], r->a[2], r->a[3], r->a[4], r->a[5], r->a[6], r->a[7]);
    return 0;
  }
  case HC_CALLN: {
    hc_fn16 f = (hc_fn16)(uintptr_t)r->a[0];
    uint64_t n = r->a[1];
    const uint64_t* v = (const uint64_t*)(uintptr_t)r->a[2];
    if (!f) return HC_EFAULT;
    if (n > HC_MAXARGS) return HC_EINVAL;
    if (n && !v) return HC_EFAULT;
    uint64_t a[HC_MAXARGS] = {0};
    for (uint64_t i = 0; i < n; i++) a[i] = v[i];
    r->result = f(a[0], a[1], a[2],  a[3],  a[4],  a[5],  a[6],  a[7],
                  a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15]);
    return 0;
  }
  default:
    return HC_ENOSYS;
  }
}
