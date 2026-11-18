# SDDM Metal depth blocker

Status: unresolved as of 2026-11-18. This records the on-screen boot of the
disposable `build/plasma-development-48d92b3-root` and the driver trace, not a
claim that SDDM is working.

`sddm-greeter-qt6` and KWin run. The foreign IOSurface handoff reports success,
KWin repeatedly submits `DRM_PRESENT_METAL` frames, and the owner presents
them. The host Shimmy window remains black. Broker pixel probes on the greeter
source and final target were both all black (`0/256` sampled pixels). This is
past service startup and transport; the source is not painted.

The greeter's Gallium framebuffer has one direct Metal color attachment
(`color_fmt=54`, `color_bind=1048586`) and a depth/stencil attachment
(`depth_fmt=146`, `depth_bind=1`). Its clear requests `buffers=7`, and its draw
has depth testing and depth writes enabled (`stencil[0/1]` disabled). The
driver logs `RDMETAL_UNSUPPORTED_CLEAR` and `RDMETAL_UNSUPPORTED_DRAW` for the
greeter PID. `rdmetal_fallback_route.h` rejects a direct color attachment
whenever `fb->zsbuf` is present. The depth resource is allocated by softpipe,
while the color resource belongs to the Metal broker, so neither backend owns
the complete framebuffer.

Do not simply ignore the depth attachment or force this mixed framebuffer
through softpipe. Both would render with incorrect depth semantics. The next
implementation needs a broker-backed depth/stencil resource with clear,
depth compare/write, lifetime and sharing behavior, and a draw protocol that
explicitly carries the relevant state. Only advertise formats and operations
whose Metal behavior has been tested. Keep unsupported cases fail closed.

Acceptance: a fresh disposable-root SDDM boot with the normal Mesa driver
paints nonblack greeter source pixels, presents them on screen, accepts login,
and returns to the greeter after logout. Also verify the greeter's depth clear
and depth-tested draws with a small focused GPU test. A nonblack frame alone
does not prove those depth semantics or establish that all composition runs on
the GPU.

Host increment: `rdmetal_resource.m` can now allocate a private Metal
Depth32Float/Stencil8 attachment alongside an image and submit fenced depth
clears. `tests/test_rdmetal_depth.m` exercises allocation, invalid input,
pending-command rejection, two clears and fence completion on the Apple GPU.
This primitive is not yet connected to the guest driver or draw packet, so it
does not change the black-screen outcome.

Broker increment: the authenticated image lease now accepts a checked
`RDGPU_CLEAR_DEPTH_STENCIL` command. The guest entry validates finite depth,
stencil, mask, image ownership and pending work; the broker checks the same
wire data, clears the host attachment and returns a fence polled by the
existing WAIT operation. Native tests cover valid, malformed and pending
requests. Mesa still does not submit this command, and its draw packets still
lack depth compare/write state, so the greeter remains black.

Resource correction: Linux depth attachments are independent resources and
may be attached to more than one color target. A per-color depth texture would
silently violate that behavior. `RDMetalDepth` now allocates a separate host
Metal texture with its own lifetime and fence. The native Apple GPU test
verifies two distinct resources and pending clear/poll behavior. The next
broker change must allocate and identify these separate depth resources, then
Mesa must attach the exact one in each draw. The existing color-image depth
primitive is not sufficient for the greeter and is not used by Mesa.

Lease increment: the broker now creates an independent `RDGPU_CREATE_DEPTH`
lease counted against the 512 MiB session quota, with a clear/poll/release path,
and no export or color-image lookup. The guest syscall has a distinct create
operation and records the depth resource type. The host GPU test covers a
3600×2338 depth allocation (the observed greeter size), a clear, wait, denied
export and release; the guest test covers creation and a checked clear through
that lease. This still does not supply a depth attachment to a Metal draw or
make Mesa create depth resources via the broker.
