# Default Metal desktop path

Current end-to-end status: **not working yet**. The user reports that the
greeter flashes and accepts no input. A captured clock/date and passing GPU
fixture tests demonstrate partial rendering progress; they do not establish a
usable greeter. Flicker, input delivery, authentication and login-to-desktop
remain unresolved or unverified.

The GUI and CLI now both compile the guest GPU syscall `0x2002`. The host establishes the local Metal broker by default; the guest no longer needs a renderer selection flag. The GUI guest environment no longer injects `LIBGL_ALWAYS_INDIRECT=1`. The virtual DRM card supports the tested master/authentication exchange Aquamarine uses when it reopens the card.

In a disposable Arch root with no `RDMETAL_*`, `MESA_*`, `LIBGL_*`, or `GALLIUM_*` selector, both surfaceless and GBM `eglinfo` reported `rdmetal Metal (direct)`. A separate guest EGL triangle produced `RDMETAL_FIXTURE_DRAW result=0` in the host log, establishing an actual Metal draw. In isolated standalone Hyprland, EGL initialized, the broker capability query succeeded, and Hyprland entered its event loop. A metadata-only broker query returned the real IOSurface stride (14464 bytes) before first render.

The staged broker/KMS integration now issues an identity-only GBM carrier before a draw, keeps its IOSurface unavailable until the Metal fence completes, then presents the completed image in the same-process primary window. An isolated standalone Omarchy run without renderer selector variables logged `DRM_PRESENT_METAL` twice for the 3600×2338 built-in display and `[present-stats] path=drm_iosurface_metal ... cpu_framebuffer_bytes=0`. This proves a Metal-backed framebuffer reached the window; it does not prove the compositor's full GLES workload or sustained frames.

The first GUI run then hit `GL_OUT_OF_MEMORY` because the Mesa sampled-resource allocator rejected large RGBA8 textures. The unified direct IOSurface resource and checked host image-upload operation cleared that allocation failure; Hyprland reported a complete monitor workbuffer (`status 36053`). The next run exposed a null `pipe_context->set_scissor_states` call; registering the callback advanced three shader variants. A later `glInvalidateFramebuffer` crash was traced to a process-global softpipe fallback pointer being destroyed by EGL's earlier probe screens. The fallback is now owned per Mesa screen. With that fixed, the latest isolated run reaches the same Metal framebuffer but stops in softpipe vertex translation: a non-direct depth/stencil framebuffer attempts to use rdmetal's custom vertex buffer as a softpipe buffer. The native depth/stencil path described below now addresses that resource boundary. Logs: `build/gpu-integrated-omarchy.log`, `build/gpu-upload-omarchy.log`, `build/gpu-scissor-omarchy.log`, `build/gpu-per-screen-omarchy.log`.

The completed broker/KMS unit tests cover stale/replayed FDs, pre-completion denial and buffer reuse. The image-upload test covers format conversion, sizes and lease/fence checks, and guest-memory copyin is range-checked. An audited Mach IOSurface transport passed a separate two-process test; the systemd greeter now renders through KWin and the host Metal window; login-to-desktop remains unverified. Remaining desktop gates include partial scissored clears, readback, sustained nonblack composition, cross-process host-window presentation, ordinary-format fallback counts, secondary display, and login-to-desktop operation.

Native depth/stencil rendering is now implemented for a single native color
attachment with a matching, single-sample depth/stencil attachment. The driver
allocates broker depth resources, waits for clears, and sends depth comparison,
depth writes, front/back stencil operations, references, masks, winding and color
write masks in V7 draw packets. OpenGL clip depth is converted to Metal's range.
V8 carries up to 4096 bytes of independent vertex and fragment constants and
runs application NIR shaders for the supported two- and three-attribute layouts.
Semantic varying locations link consistently, including GLES2 gl_FragColor.
Default uniform arrays receive an explicit Metal buffer binding, avoiding
collision with vertex data. Dead dereferences left by uniform lowering are
removed before SPIR-V emission. Mixed native/software attachments remain rejected.

`python3 tests/run-depth-stencil-egl.py` checks the installed aarch64 driver through
guest EGL and verifies actual broker pixels: red survives a depth rejection and
a stencil rejection, then changes to green after the stencil clear. Additional draws verify an alpha-only
glyph texture, a partial atlas upload, real position/UV/opacity shaders with vertex/fragment uniforms, and
indexed triangle strips and fans.
`bash tests/run-depth-stencil.sh` covers native GPU occlusion, stencil clipping,
front/back state, masks, clears, attachment validation and truncated packets.
These establish the supported rendering path; full desktop verification is a
separate gate. MSAA, multiple color attachments, partial scissored clears and
native depth readback are outside this path.

DRM connector discovery uses the host display's online state, so a sleeping
built-in panel retains its mode. The observed sleeping panel was online at
3600x2338 while CoreGraphics reported it inactive; testing confirmed that the
corrected connector remained connected. Physically absent displays still have
no advertised mode.

The full boot verification captured a readable, upright greeter clock/date in
`build/greeter-rendering.png`, with successful V8 application-shader draws and
KWin Metal-backed frame presentation. The emitter also supports the pow and
saturate operations used by Qt shaders. The subsequent user run reports flashing and no input response. The login
interaction is not working, and authentication and a desktop session have not
been verified.
