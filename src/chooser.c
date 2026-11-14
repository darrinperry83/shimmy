// chooser.c — shimmy boot chooser (on-screen render + direct-scanout present).
//
// Runs from launchd BEFORE WindowServer (LaunchDaemon com.shimmy.chooser, which
// registers the com.apple.windowserver bootstrap service). It draws a 1-line boot
// menu straight to the builtin display's scanout surface via the private CoreDisplay
// present path, reads a key from HID (or the console), and either:
//   * key 1/m/Enter/timeout  -> boot macOS (kickstart the real WindowServer), or
//   * key 2/l/s              -> boot shimmy/Linux (fork+exec the stand-in WindowServer).
//
// NOTE ON RECONSTRUCTION: the source repo this file originally lived in
// (/tmp/rmatrixos-shim) was wiped before this task ran; only the installed binary
// /usr/local/sbin/shimmy-chooser remained. This file was reconstructed from that
// binary (full disassembly + string/symbol table, see /tmp/chooser-re) and from the
// reference present code (shim-metal-clear src/window.m, src/rdgpu_scanout_client.m,
// direct-display-probe/direct_display_probe.m), then FIXED per the boot log.

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOSurface/IOSurface.h>

#include <servers/bootstrap.h>
#include <mach/mach.h>
#include <mach/task_special_ports.h>

#include <dlfcn.h>
#include <signal.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "chooser_font.h"

// ---------------------------------------------------------------------------------------------
// Present state (the private CoreDisplay scanout hops).
typedef struct __CoreDisplay        *CoreDisplayRef;
typedef struct __CoreDisplaySurface *CoreDisplaySurfaceRef;

typedef void*   (*fn_CGXDisplayDeviceForDisplayID)(CGDirectDisplayID);
typedef CoreDisplayRef (*fn_GetDisplayForCGXDisplayDevice)(void*);
typedef CoreDisplaySurfaceRef (*fn_GetNextSurface)(CoreDisplayRef);
typedef IOSurfaceRef (*fn_SurfaceGetIOSurface)(CoreDisplaySurfaceRef);
typedef int     (*fn_PresentIOSurface)(CoreDisplayRef, IOSurfaceRef);

static void*          g_core_display;      // CoreDisplay dylib handle
static int            g_cd_resolved;       // 1 once dlsym'd
static fn_CGXDisplayDeviceForDisplayID   f_cgx;
static fn_GetDisplayForCGXDisplayDevice  f_get_display;
static fn_GetNextSurface                 f_get_next;
static fn_SurfaceGetIOSurface            f_surface_get_io;
static fn_PresentIOSurface               f_present_io;

// Fault guard: the private CGX hop can branch through a NULL fn pointer when the
// process has no live CoreDisplay master connection; catch it so a bad hop does not
// crash the chooser at boot.
static sigjmp_buf        g_present_jmp;
static volatile sig_atomic_t g_present_guarded;

// HID state.
static IOHIDManagerRef   g_hid;             // IOHIDManager (may be NULL)
static int               g_hid_ready;       // 1 once IOHIDManagerOpen succeeded
static int               g_key_queue[64];   // ring of ASCII keys from the HID callback
static int               g_key_count;       // queued keys
static int               g_key_head;        // next write slot
static int               g_key_tail;        // next read slot
static int               g_console_fd = 0;  // fd the console fallback reads (0 = stdin)

// ---------------------------------------------------------------------------------------------
// Small helpers.

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

// Resolve the MAIN display id. In the WindowServer-less boot context CGMainDisplayID()
// can return 0 (the "main" flag lives in WindowServer's display list, which is not up
// yet); fall back to the first CGGetActiveDisplayList entry, preferring the one that
// CGDisplayIsMain() flags. This is the display-id fix: the previous code read
// CGMainDisplayID() only and, when it returned 0, mapped "display id 0x00000000" and
// failed to present.
static CGDirectDisplayID main_display_id(void) {
    CGDirectDisplayID main = CGMainDisplayID();
    if (main != 0) return main;

    CGDirectDisplayID ds[16];
    uint32_t n = 0;
    if (CGGetActiveDisplayList(16, ds, &n) == kCGErrorSuccess && n > 0) {
        for (uint32_t i = 0; i < n; i++)
            if (CGDisplayIsMain(ds[i])) return ds[i];
        return ds[0];
    }
    return 0;
}

// Pick a reachable console fd and put a tty into raw mode so keypresses are delivered
// one byte at a time (no canonical line buffering, no echo wait). StandardInPath is
// /dev/console, which on a graphical boot is frequently NOT a tty; opening /dev/console
// or /dev/tty directly (and forcing raw mode) is the fallback that actually delivers
// the 1/2/m/l key at boot.
static int hid_console_fallback(void) {
    int fd = 0;                                   // stdin = StandardInPath=/dev/console
    if (!isatty(fd)) {
        int c = open("/dev/console", O_RDONLY | O_NONBLOCK);
        if (c < 0) c = open("/dev/tty", O_RDONLY | O_NONBLOCK);
        if (c >= 0) fd = c;
    }
    if (isatty(fd)) {
        struct termios t;
        if (tcgetattr(fd, &t) == 0) {
            cfmakeraw(&t);
            tcsetattr(fd, TCSANOW, &t);
        }
    }
    return fd;
}

static int hid_push_key(int key) {
    if (g_key_count > 63) return 0;
    g_key_queue[g_key_head] = key;
    g_key_head = (g_key_head + 1) & 63;
    g_key_count++;
    return 1;
}

static int hid_pop_key(void) {
    if (g_key_count <= 0) return -1;
    int key = g_key_queue[g_key_tail];
    g_key_tail = (g_key_tail + 1) & 63;
    g_key_count--;
    return key;
}

// ---------------------------------------------------------------------------------------------
// Present: map a public CGDirectDisplayID -> opaque CoreDisplayRef.

static CoreDisplayRef display_ref_for_id(CGDirectDisplayID publicID) {
    if (sigsetjmp(g_present_jmp, 1) == 0) {
        g_present_guarded = 1;
        void* dev = f_cgx(publicID);
        if (!dev) {
            g_present_guarded = 0;
            return NULL;
        }
        CoreDisplayRef ref = f_get_display(dev);
        g_present_guarded = 0;
        return ref;
    }
    // Faulted: the CGX hop is not reachable from this process context.
    g_present_guarded = 0;
    return NULL;
}

// Resolve the CoreDisplay dylib + the three present hops. Returns 0 when
// CGXDisplayDeviceForDisplayID, CoreDisplay_GetDisplayForCGXDisplayDevice and
// CoreDisplay_Display_PresentIOSurface are all resolved.
static int present_resolve(void) {
    if (g_cd_resolved) return g_core_display ? 0 : -1;
    g_cd_resolved = 1;

    g_core_display = dlopen("/System/Library/Frameworks/CoreDisplay.framework/CoreDisplay", RTLD_LAZY);
    if (!g_core_display) g_core_display = dlopen("CoreDisplay", RTLD_LAZY);
    if (!g_core_display) return -1;

    f_cgx          = (fn_CGXDisplayDeviceForDisplayID)dlsym(g_core_display, "CGXDisplayDeviceForDisplayID");
    f_get_display  = (fn_GetDisplayForCGXDisplayDevice)dlsym(g_core_display, "CoreDisplay_GetDisplayForCGXDisplayDevice");
    f_get_next     = (fn_GetNextSurface)dlsym(g_core_display, "CoreDisplay_Display_GetNextSurface");
    f_surface_get_io = (fn_SurfaceGetIOSurface)dlsym(g_core_display, "CoreDisplay_DisplaySurface_GetIOSurface");
    f_present_io   = (fn_PresentIOSurface)dlsym(g_core_display, "CoreDisplay_Display_PresentIOSurface");

    if (f_cgx && f_get_display && f_present_io) return 0;
    return -1;
}

static void present_guard_handler(int sig) {
    (void)sig;
    if (g_present_guarded) siglongjmp(g_present_jmp, 1);
    signal(sig, SIG_DFL);
    raise(sig);
}

// Present the rendered menu framebuffer straight to the MAIN display's scanout.
static int present_framebuffer(const uint8_t* fb, uint32_t width, uint32_t height) {
    if (present_resolve() != 0) {
        fwrite("chooser: present: CoreDisplay present symbols unavailable\n", 1, 58, stderr);
        return -1;
    }

    // Display-id fix: resolve the MAIN display id (CGMainDisplayID -> first active
    // list entry = 0x1), never 0x0.
    CGDirectDisplayID displayID = main_display_id();

    CoreDisplayRef ref = display_ref_for_id(displayID);
    if (!ref) {
        fprintf(stderr, "chooser: present: could not map display id 0x%08x -> CoreDisplayRef\n",
                (unsigned)displayID);
        return -1;
    }

    // Prefer the display's own "next" scanout surface (write straight into it); if that
    // is not reachable, allocate a fresh BGRA IOSurface for the menu frame.
    IOSurfaceRef surface = NULL;
    int used_display_next = 0;
    if (f_get_next && f_surface_get_io) {
        if (sigsetjmp(g_present_jmp, 1) == 0) {
            g_present_guarded = 1;
            CoreDisplaySurfaceRef next = f_get_next(ref);
            IOSurfaceRef io = next ? f_surface_get_io(next) : NULL;
            g_present_guarded = 0;
            if (io) { surface = io; used_display_next = 1; }
        } else {
            g_present_guarded = 0;
        }
    }

    CFNumberRef cfW = NULL, cfH = NULL, cfBPR = NULL, cfBPE = NULL, cfFmt = NULL;
    CFMutableDictionaryRef props = NULL;
    if (!surface) {
        int32_t w = (int32_t)width, h = (int32_t)height;
        int32_t bpr = w * 4, bpe = 4, fmt = (int32_t)'BGRA';
        cfW   = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &w);
        cfH   = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &h);
        cfBPR = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &bpr);
        cfBPE = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &bpe);
        cfFmt = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &fmt);
        if (cfW && cfH && cfBPR && cfBPE && cfFmt) {
            props = CFDictionaryCreateMutable(kCFAllocatorDefault, 5,
                                              &kCFTypeDictionaryKeyCallBacks,
                                              &kCFTypeDictionaryValueCallBacks);
        }
        if (props) {
            CFDictionarySetValue(props, kIOSurfaceWidth, cfW);
            CFDictionarySetValue(props, kIOSurfaceHeight, cfH);
            CFDictionarySetValue(props, kIOSurfaceBytesPerRow, cfBPR);
            CFDictionarySetValue(props, kIOSurfaceBytesPerElement, cfBPE);
            CFDictionarySetValue(props, kIOSurfacePixelFormat, cfFmt);
            surface = IOSurfaceCreate(props);
        }
    }
    if (cfW) CFRelease(cfW);
    if (cfH) CFRelease(cfH);
    if (cfBPR) CFRelease(cfBPR);
    if (cfBPE) CFRelease(cfBPE);
    if (cfFmt) CFRelease(cfFmt);
    if (props) CFRelease(props);

    if (!surface) {
        fwrite("chooser: present: no IOSurface to write\n", 1, 40, stderr);
        return -1;
    }

    size_t surfW = IOSurfaceGetWidth(surface);
    size_t surfH = IOSurfaceGetHeight(surface);
    size_t surfBPR = IOSurfaceGetBytesPerRow(surface);
    size_t rows = surfH < height ? surfH : height;
    size_t cols = surfW < width ? surfW : width;

    int rc = -1;
    if (IOSurfaceLock(surface, 0, NULL) == kIOReturnSuccess) {
        uint8_t* dst = (uint8_t*)IOSurfaceGetBaseAddress(surface);
        if (dst && rows > 0 && cols > 0) {
            for (size_t y = 0; y < rows; y++) {
                const uint8_t* src = fb + (size_t)y * width * 4;
                uint8_t* d = dst + y * surfBPR;
                memcpy(d, src, cols * 4);
            }
        }
        IOSurfaceUnlock(surface, 0, NULL);

        const char* which = used_display_next ? "display-next" : "fresh";
        if (sigsetjmp(g_present_jmp, 1) == 0) {
            g_present_guarded = 1;
            rc = f_present_io(ref, surface);
            g_present_guarded = 0;
        } else {
            g_present_guarded = 0;
            rc = -1;
        }
        fprintf(stderr, "chooser: present: %s %zux%zu surface to display 0x%08x -> rc=%d\n",
                which, surfW, surfH, (unsigned)displayID, rc);
    }

    if (!used_display_next) CFRelease(surface);
    return rc;
}

// ---------------------------------------------------------------------------------------------
// HID / keyboard.

// IOHIDValueCallback: key-down (integer value 1) on the keyboard page -> ASCII key into
// the ring queue. Usage -> char map mirrors the original binary's jump table:
//   0x0f L -> 'l', 0x10 M -> 'm', 0x16 S -> 's', 0x1e 1 -> '1', 0x1f 2 -> '2',
//   0x28 Enter -> '0', 0x29 Esc -> '\r', 0x4e -> '\r', 0x4f -> '1',
//   0x50 -> '2', 0x61 -> '0'; every other usage is ignored.
static void hid_value_cb(void* ctx, IOReturn result, void* sender, IOHIDValueRef value) {
    (void)ctx; (void)sender;
    if (result) return;
    IOHIDElementRef el = IOHIDValueGetElement(value);
    if (!el) return;
    if (IOHIDValueGetIntegerValue(value) != 1) return;          // press only
    if (IOHIDElementGetUsagePage(el) != kHIDPage_KeyboardOrKeypad) return;

    uint32_t usage = (uint32_t)IOHIDElementGetUsage(el);
    if (usage < 0x0f || usage > 0x62) return;

    char c;
    switch (usage) {
        case 0x0f: c = 'l'; break;
        case 0x10: c = 'm'; break;
        case 0x16: c = 's'; break;
        case 0x1e: c = '1'; break;
        case 0x1f: c = '2'; break;
        case 0x28: c = '0'; break;   // Enter -> macOS default
        case 0x29: c = '\r'; break;  // Esc   -> macOS default
        case 0x4e: c = '\r'; break;
        case 0x4f: c = '1'; break;
        case 0x50: c = '2'; break;
        case 0x61: c = '0'; break;
        default:   return;           // ignore unmapped keys
    }
    hid_push_key((unsigned char)c);
}

static void hid_init(void) {
    if (g_hid) return;

    g_hid = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!g_hid) {
        fwrite("chooser: hid: IOHIDManagerCreate failed\n", 1, 40, stderr);
        return;
    }

    int32_t usage_page = kHIDPage_GenericDesktop;   // 1
    int32_t usage      = kHIDUsage_GD_Keyboard;      // 6
    CFNumberRef cfPage = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &usage_page);
    CFNumberRef cfUse  = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &usage);
    CFMutableDictionaryRef match = NULL;
    if (cfPage && cfUse) {
        match = CFDictionaryCreateMutable(kCFAllocatorDefault, 2,
                                          &kCFTypeDictionaryKeyCallBacks,
                                          &kCFTypeDictionaryValueCallBacks);
    }
    if (match) {
        CFDictionarySetValue(match, CFSTR("DeviceUsagePage"), cfPage);
        CFDictionarySetValue(match, CFSTR("DeviceUsage"), cfUse);
        IOHIDManagerSetDeviceMatching(g_hid, match);
    }
    if (cfPage) CFRelease(cfPage);
    if (cfUse) CFRelease(cfUse);
    if (match) CFRelease(match);

    IOHIDManagerRegisterInputValueCallback(g_hid, hid_value_cb, NULL);
    IOHIDManagerScheduleWithRunLoop(g_hid, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);

    IOReturn ret = IOHIDManagerOpen(g_hid, kIOHIDOptionsTypeNone);
    if (ret != kIOReturnSuccess) {
        g_hid_ready = 0;
        fprintf(stderr, "chooser: hid: IOHIDManagerOpen failed (0x%x); /dev/console fallback active\n",
                (unsigned)ret);
        // IOHIDManagerOpen returns kIOReturnNotPermitted (0xe00002e2) when this daemon
        // lacks the Input Monitoring TCC grant. Grant it via System Settings >
        // Privacy & Security > Input Monitoring (or a TCC.db / MDM grant for the
        // LaunchDaemon) to restore the HID path; until then the console/tty fallback
        // below delivers the 1/2/m/l key.
        if (ret == (IOReturn)kIOReturnNotPermitted)
            fprintf(stderr, "chooser: hid: IOHIDManagerOpen -> kIOReturnNotPermitted: "
                    "grant Input Monitoring to the chooser daemon (TCC) to enable HID input\n");
        g_console_fd = hid_console_fallback();
        return;
    }

    g_hid_ready = 1;
    CFSetRef devices = IOHIDManagerCopyDevices(g_hid);
    if (devices) {
        fprintf(stderr, "chooser: hid: IOHIDManager open, %ld keyboard device(s) matched\n",
                (long)CFSetGetCount(devices));
        CFRelease(devices);
    } else {
        fprintf(stderr, "chooser: hid: IOHIDManager open, %ld keyboard device(s) matched\n", 0L);
    }
}

// ---------------------------------------------------------------------------------------------
// Render.

static void draw_text(uint8_t* fb, uint32_t width, uint32_t height,
                      uint32_t x_center, uint32_t y, uint32_t scale,
                      const char* text, uint32_t color) {
    size_t n = strlen(text);
    int32_t start_x = (int32_t)x_center - (int32_t)(n * 4 * scale);  // n * 8 * scale / 2
    uint8_t b = (uint8_t)(color & 0xff);
    uint8_t g = (uint8_t)((color >> 8) & 0xff);
    uint8_t r = (uint8_t)((color >> 16) & 0xff);

    for (size_t ci = 0; ci < n; ci++) {
        unsigned char ch = (unsigned char)text[ci];
        int gi = (ch >= 32 && ch < 127) ? (ch - 32) : ('?' - 32);
        const uint8_t* glyph = &chooser_font8x13[gi * 13];
        for (int row = 0; row < 13; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; col++) {
                if (!(bits & (0x80u >> col))) continue;
                int32_t px = start_x + (int32_t)(ci * 8 * scale) + (int32_t)(col * scale);
                int32_t py = (int32_t)y + (int32_t)(row * scale);
                for (uint32_t sy = 0; sy < scale; sy++) {
                    for (uint32_t sx = 0; sx < scale; sx++) {
                        int32_t X = px + (int32_t)sx;
                        int32_t Y = py + (int32_t)sy;
                        if (X < 0 || Y < 0 || X >= (int32_t)width || Y >= (int32_t)height) continue;
                        uint8_t* p = fb + ((size_t)Y * width + (size_t)X) * 4;
                        p[0] = b; p[1] = g; p[2] = r; p[3] = 0xff;
                    }
                }
            }
        }
    }
}

// Render the boot menu into a BGRA framebuffer (opaque black background).
static void render_menu(uint8_t* fb, uint32_t width, uint32_t height,
                        uint32_t scale, int timeout) {
    // Fill opaque black.
    for (uint32_t i = 0; i < width * height; i++) {
        uint32_t* px = (uint32_t*)fb + i;
        *px = 0xff000000u;   // B=0,G=0,R=0,A=0xff
    }

    uint32_t x_center = width / 2;
    uint32_t y_top = (height - 80u * scale) / 2u;

    char line[64];
    snprintf(line, sizeof line, "(defaults to macOS in %d s)", timeout);

    draw_text(fb, width, height, x_center, y_top, scale, "shimmy", 0x00ffffffu);                 // title
    draw_text(fb, width, height, x_center, y_top + 16u * scale, scale, "[1] Boot macOS", 0x00ffffffu);
    draw_text(fb, width, height, x_center, y_top + 32u * scale, scale, "[2] Boot shimmy (Linux)", 0x0055ff55u);
    draw_text(fb, width, height, x_center, y_top + 64u * scale, scale, line, 0x00aaaaaau);
}

// --render <path.ppm> [--render-size WxH]: render the menu to a PPM and exit.
static int render_mode(const char* path, uint32_t width, uint32_t height) {
    int timeout = 10;
    const char* te = getenv("SHIMMY_BOOT_TIMEOUT");
    if (te && te[0]) {
        char* end = NULL;
        long v = strtol(te, &end, 10);
        if (end && *end == '\0' && v > 0) timeout = (int)v;
    }
    uint32_t scale = width > 0x59f ? (width > 0x9ff ? 4 : 3) : 2;

    uint8_t* fb = calloc((size_t)width * height, 4);
    if (!fb) {
        fwrite("chooser: render alloc failed\n", 1, 29, stderr);
        return 1;
    }

    render_menu(fb, width, height, scale, timeout);

    FILE* f = fopen(path, "wb");
    if (!f) {
        perror("chooser: render write");
        free(fb);
        return 1;
    }
    fprintf(f, "P6\n%d %d\n255\n", (int)width, (int)height);
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* row = fb + (size_t)y * width * 4;
        for (uint32_t x = 0; x < width; x++) {
            const uint8_t* p = row + (size_t)x * 4;
            fputc(p[0], f);   // B
            fputc(p[1], f);   // G
            fputc(p[2], f);   // R
        }
    }
    fclose(f);

    size_t lit = 0;
    for (uint32_t i = 0; i < width * height; i++) {
        const uint8_t* p = fb + (size_t)i * 4;
        if (p[0] || p[1] || p[2]) lit++;
    }
    printf("render: wrote %s (%dx%d, scale %d, countdown %d s, %zu lit pixels)\n",
           path, (int)width, (int)height, (int)scale, timeout, lit);
    printf("render: menu text:\n  shimmy\n  [1] Boot macOS\n  [2] Boot shimmy (Linux)\n  (defaults to macOS in %d s)\n",
           timeout);
    free(fb);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Branch execution.

static void branch_macos(int kickstart, int dry_run);

// SHIMMY_PRESENT=1 (or --present) enables the live direct-scanout present.
static int present_enabled_flag(int flag) {
    if (flag) return 1;
    const char* e = getenv("SHIMMY_PRESENT");
    if (e && e[0] == '1' && e[1] == '\0') return 1;
    return 0;
}

static void branch_shimmy(int dry_run) {
    const char* cmd = getenv("SHIMMY_WINDOWSERVER_CMD");
    const char* standin = (cmd && cmd[0]) ? cmd : "/usr/local/sbin/shim_windowserver";

    const char* sys = getenv("SHIMMY_WINDOWSERVER_SYSTEM");
    int test_names = (sys && sys[0] == '0' && sys[1] == '\0');

    const char* ws = getenv("SHIMMY_WARM_SWITCH_KICKSTART");
    int kickstart = (ws && ws[0] == '1' && ws[1] == '\0');

    puts("chooser: branch = shimmy (takeover)");
    printf("chooser: stand-in = %s (%s)\n", standin,
           test_names ? "test-name mode (safe: no real check-in)"
                      : "RDNATIVE_WINDOWSERVER_SYSTEM=1 -> real check-in");

    if (dry_run) {
        printf("chooser: dry-run: would fork+exec %s with RDNATIVE_WINDOWSERVER_SYSTEM=1 so it\n", standin);
        printf("chooser: dry-run:   checks in %s then %s and runs the ~37-function server,\n",
               "com.apple.windowserver", "com.apple.windowserver.active");
        puts("chooser: dry-run:   then on clean exit / macOS key (1/m) warm-switch to the real WindowServer");
        printf("chooser: dry-run:   warm-switch kickstart: %s\n",
               kickstart ? "launchctl kickstart -k system/com.apple.WindowServer" : "disabled (print only)");
        return;
    }

    if (access(standin, X_OK) != 0) {
        fprintf(stderr, "chooser: stand-in not executable: %s (%s); fail-safe -> macOS\n",
                standin, strerror(errno));
        branch_macos(kickstart, 0);
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("chooser: fork");
        branch_macos(kickstart, 0);
        return;
    }
    if (pid == 0) {
        if (!test_names) setenv("RDNATIVE_WINDOWSERVER_SYSTEM", "1", 1);
        execl(standin, standin, (char*)NULL);
        perror("chooser: execl stand-in");
        _exit(127);
    }

    printf("chooser: stand-in pid %d running (macOS key 1/m or SIGTERM to warm-switch)\n", pid);
    fflush(stdout);

    int status = 0;
    pid_t w = waitpid(pid, &status, WNOHANG);
    while (w != pid) {
        if (w < 0) {
            if (errno == EINTR) {
                w = waitpid(pid, &status, WNOHANG);
                continue;
            }
            perror("chooser: waitpid");
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            w = pid;
            break;
        }

        struct pollfd pfd = { .fd = g_console_fd, .events = POLLIN, .revents = 0 };
        if (poll(&pfd, 1, 250) == 1) {
            char c;
            if (read(g_console_fd, &c, 1) == 1) {
                int off = (unsigned char)c - '0';
                if (off >= 0 && off <= 61) {
                    if ((1ULL << off) & 0x2000000020000003ULL) {   // bits 0,1,29,61 -> '0','1','M','m'
                        printf("chooser: macOS key '%c' -> warm-switch (SIGTERM to stand-in %d)\n", c, pid);
                        fflush(stdout);
                        if (kill(pid, SIGTERM) != 0 && errno != ESRCH) perror("chooser: kill(SIGTERM)");
                        int waited_ms = 0;
                        for (;;) {
                            w = waitpid(pid, &status, WNOHANG);
                            if (w == pid) break;
                            if (w < 0 && errno != EINTR) break;
                            if (waited_ms >= 5000) {
                                fwrite("chooser: stand-in did not exit after SIGTERM; sending SIGKILL\n", 1, 62, stderr);
                                kill(pid, SIGKILL);
                                waitpid(pid, &status, 0);
                                w = pid;
                                break;
                            }
                            usleep(100000);
                            waited_ms += 100;
                        }
                        break;
                    }
                }
            }
        }
        w = waitpid(pid, &status, WNOHANG);
    }

    int sig = status & 0x7f;
    int code = (status >> 8) & 0xff;
    if (sig == 0) printf("chooser: stand-in exited (status %d)\n", code);
    else if (sig != 0x7f) printf("chooser: stand-in terminated by signal %d\n", sig);
    if ((code | sig) != 0)
        fwrite("chooser: stand-in did not exit cleanly; fail-safe -> macOS\n", 1, 59, stderr);

    puts("chooser: warm-switch: handing back to the real WindowServer");
    branch_macos(kickstart, 0);
}

static void branch_macos(int kickstart, int dry_run) {
    puts("chooser: branch = macOS (normal boot)");
    if (dry_run) {
        if (kickstart) puts("chooser: dry-run: would kickstart system/com.apple.WindowServer");
        return;
    }
    if (kickstart) {
        puts("chooser: kickstarting system/com.apple.WindowServer");
        int rc = system("/bin/launchctl kickstart -k system/com.apple.WindowServer");
        if (rc) fprintf(stderr, "chooser: kickstart returned %d (ignored)\n", rc);
    }
}

// ---------------------------------------------------------------------------------------------
// Branch resolution + interactive loop.

// Interpret an ASCII key: returns 1 = shimmy, 0 = macOS.
static int key_branch(int key) {
    const char* label;
    int branch;
    if (key == '2') {
        branch = 1;
        label = "shimmy";
    } else {
        int shift = (unsigned char)key - 'L';
        int bit = (shift <= 39) ? (int)((1ULL << shift) & 0x0000008100000081ULL) : 0;  // 'L','S','l','s'
        branch = bit ? 1 : 0;
        label = bit ? "shimmy" : "macOS";
    }
    printf("chooser: key '%c' -> %s\n", (char)key, label);
    fflush(stdout);
    return branch;
}

static int resolve_branch(const char* choice, int present_flag) {
    // Forced choice (--choice / SHIMMY_BOOT_CHOICE).
    if (choice) return strcmp(choice, "shimmy") == 0;
    const char* bc = getenv("SHIMMY_BOOT_CHOICE");
    if (bc && bc[0]) return strcmp(bc, "shimmy") == 0;

    int timeout = 10;
    const char* te = getenv("SHIMMY_BOOT_TIMEOUT");
    if (te && te[0]) {
        char* end = NULL;
        long v = strtol(te, &end, 10);
        if (end && *end == '\0' && v > 0) timeout = (int)v;
    }

    hid_init();

    // Framebuffer size: use the MAIN display's native size when presenting.
    uint32_t width = 960, height = 540;
    uint32_t scale = 2;
    if (present_flag) {
        CGDirectDisplayID mainID = main_display_id();
        if (mainID != 0) {
            size_t w = CGDisplayPixelsWide(mainID);
            size_t h = CGDisplayPixelsHigh(mainID);
            if (w >= 1 && w <= 0x4000 && h >= 1 && h <= 0x4000) {
                width = (uint32_t)w;
                height = (uint32_t)h;
                scale = w > 0x59f ? (w > 0x9ff ? 4 : 3) : 2;
            }
        }
    }

    uint8_t* fb = calloc((size_t)width * height, 4);
    if (!fb) {
        fprintf(stderr, "chooser: menu framebuffer alloc failed (%dx%d); macOS default\n",
                (int)width, (int)height);
        return 0;
    }

    render_menu(fb, width, height, scale, timeout);
    if (present_flag) {
        int rc = present_framebuffer(fb, width, height);
        if (rc)
            fprintf(stderr, "chooser: menu present failed (rc=%d); keyboard still live\n", rc);
    }

    uint64_t deadline_ms = monotonic_ms() + (uint64_t)timeout * 1000u;
    int last_countdown = timeout;

    for (;;) {
        int key = -1;
        if (g_key_count > 0) {
            key = hid_pop_key();
        } else if (g_hid_ready) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.0005, true);
            if (g_key_count > 0) key = hid_pop_key();
        }
        if (key < 0) {
            struct pollfd pfd = { .fd = g_console_fd, .events = POLLIN, .revents = 0 };
            if (poll(&pfd, 1, 0) == 1) {
                unsigned char c;
                if (read(g_console_fd, &c, 1) == 1) key = c;
            }
        }
        if (key >= 0) {
            int branch = key_branch(key);
            free(fb);
            return branch;
        }

        uint64_t now = monotonic_ms();
        if (now >= deadline_ms) {
            puts("chooser: timeout / no input -> macOS");
            free(fb);
            return 0;
        }
        int remaining = (int)((deadline_ms + 999 - now) / 1000);
        if (remaining != last_countdown) {
            last_countdown = remaining;
            render_menu(fb, width, height, scale, remaining);
            if (present_flag) present_framebuffer(fb, width, height);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// main.

static void usage(FILE* out) {
    fprintf(out,
        "usage: chooser [--dry-run] [--choice macOS|shimmy] [--kickstart-windowserver]\n"
        "               [--present] [--probe-present] [--probe-hid]\n"
        "               [--render <path.ppm>] [--render-size WxH]\n"
        "               [--checkin <name> [seconds]]\n"
        "  --dry-run               print the chosen branch + full plan, touch nothing\n"
        "  --choice <branch>       force a branch (headless testing); macOS is the fallback\n"
        "  --kickstart-windowserver  on the macOS branch, kickstart system/com.apple.WindowServer\n"
        "  --present               enable the live direct-scanout present (equiv SHIMMY_PRESENT=1)\n"
        "  --probe-present         READ-ONLY: dlsym the CoreDisplay present symbols + list displays\n"
        "  --probe-hid             READ-ONLY: open IOHIDManager and enumerate keyboards\n"
        "  --render <path.ppm>     render the menu to a PPM (captured framebuffer) and exit\n"
        "  --render-size WxH       framebuffer size for --render (default %dx%d)\n"
        "  --checkin <name> [s]    test hook: bootstrap_check_in ONE name, hold it `s` seconds\n"
        "env: SHIMMY_BOOT_TIMEOUT (default 10), SHIMMY_BOOT_CHOICE, SHIMMY_PRESENT (0/1)\n"
        "     SHIMMY_WINDOWSERVER_CMD     stand-in path (default %s)\n"
        "     SHIMMY_WINDOWSERVER_SYSTEM  1 (default) real check-in; 0 = test names (safe)\n"
        "     SHIMMY_WARM_SWITCH_KICKSTART 0 (default) print only; 1 = really kickstart\n",
        960, 540, "/usr/local/sbin/shim_windowserver");
}

int main(int argc, char** argv) {
    setbuf(stdout, NULL);

    int dry_run = 0;
    int present_flag = 0;
    int kickstart_flag = 0;
    int probe_present = 0;
    int probe_hid = 0;
    const char* choice = NULL;
    const char* render_path = NULL;
    const char* checkin_name = NULL;
    int checkin_seconds = 30;
    int width = 960, height = 540;

    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (!strcmp(a, "--dry-run") || (a[0] == '-' && a[1] == 'n' && a[2] == '\0')) {
            dry_run = 1;
        } else if (!strcmp(a, "--choice")) {
            if (i + 1 < argc) { choice = argv[++i]; }
        } else if (!strcmp(a, "--kickstart-windowserver")) {
            kickstart_flag = 1;
        } else if (!strcmp(a, "--present")) {
            present_flag = 1;
        } else if (!strcmp(a, "--probe-present")) {
            probe_present = 1;
        } else if (!strcmp(a, "--probe-hid")) {
            probe_hid = 1;
        } else if (!strcmp(a, "--render")) {
            if (i + 1 < argc) render_path = argv[++i];
        } else if (!strcmp(a, "--render-size")) {
            if (i + 1 < argc) {
                int w = 0, h = 0;
                if (sscanf(argv[++i], "%dx%d", &w, &h) == 2 && w >= 1 && h >= 1 &&
                    w <= 0x4000 && h <= 0x4000) {
                    width = w; height = h;
                } else {
                    fprintf(stderr, "chooser: bad --render-size (want WxH): %s\n", a);
                    usage(stderr);
                    return 2;
                }
            }
        } else if (!strcmp(a, "--checkin")) {
            if (i + 1 < argc) checkin_name = argv[++i];
            if (i + 1 < argc) {
                const char* s = argv[i + 1];
                if (s[0] != '-') { checkin_seconds = atoi(s); i++; }
            }
        } else if (!strcmp(a, "--help") || (a[0] == '-' && a[1] == 'h' && a[2] == '\0')) {
            usage(stdout);
            return 0;
        } else {
            fprintf(stderr, "chooser: unknown argument: %s\n", a);
            usage(stderr);
            return 2;
        }
    }

    // Fault guard for the private CoreDisplay hops.
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = present_guard_handler;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    // --checkin test hook.
    if (checkin_name) {
        mach_port_t bp = MACH_PORT_NULL;
        kern_return_t kr = task_get_special_port(mach_task_self(), TASK_BOOTSTRAP_PORT, &bp);
        if (kr != KERN_SUCCESS) {
            fprintf(stderr, "checkin: task_get_bootstrap_port failed: %s\n", mach_error_string(kr));
            return 1;
        }
        mach_port_t port = MACH_PORT_NULL;
        kern_return_t ck = bootstrap_check_in(bp, (char*)checkin_name, &port);
        printf("checkin: bootstrap_check_in(%s) -> kr=%d (%s) port=0x%x\n",
               checkin_name, ck, bootstrap_strerror(ck), port);
        fflush(stdout);
        if (ck == KERN_SUCCESS) {
            printf("checkin: holding %s for %d s (verify with: launchctl print ...)\n",
                   checkin_name, checkin_seconds);
            fflush(stdout);
            sleep(checkin_seconds);
            return 0;
        }
        return 1;
    }

    // --probe-present: READ-ONLY dlsym + display list.
    if (probe_present) {
        present_resolve();
        printf("present: CoreDisplay dlopen -> %s\n", g_core_display ? "OK" : "FAILED");
        const char* syms[] = {
            "CGXDisplayDeviceForDisplayID",
            "CoreDisplay_GetDisplayForCGXDisplayDevice",
            "CoreDisplay_Display_GetNextSurface",
            "CoreDisplay_DisplaySurface_GetIOSurface",
            "CoreDisplay_Display_PresentIOSurface",
            "CoreDisplay_Display_PresentIOSurfaceWithSrcAndDstRect",
        };
        for (size_t i = 0; i < sizeof(syms)/sizeof(syms[0]); i++) {
            void* p = g_core_display ? dlsym(g_core_display, syms[i]) : NULL;
            printf("present:   %-52s %s\n", syms[i], p ? "present" : "MISSING");
        }
        printf("present: all three present hops resolved -> %s\n",
               (f_cgx && f_get_display && f_present_io) ? "yes" : "no");

        CGDirectDisplayID ds[16];
        uint32_t n = 0;
        CGError err = CGGetActiveDisplayList(16, ds, &n);
        printf("present: CGGetActiveDisplayList -> %d display(s) (err %d)\n", (int)n, (int)err);
        for (uint32_t i = 0; i < n && i < 16; i++) {
            printf("present:   display[%u] id=0x%08x %zux%zu online=%d main=%d\n",
                   i, (unsigned)ds[i], CGDisplayPixelsWide(ds[i]), CGDisplayPixelsHigh(ds[i]),
                   (int)CGDisplayIsOnline(ds[i]), (int)CGDisplayIsMain(ds[i]));
        }
        printf("present: main display id = 0x%08x\n", (unsigned)main_display_id());
        return 0;
    }

    // --probe-hid: READ-ONLY HID open + enumerate.
    if (probe_hid) {
        hid_init();
        printf("hid: IOHIDManager %s\n", g_hid_ready ? "open" : "unavailable");
        if (g_hid) {
            CFSetRef devices = IOHIDManagerCopyDevices(g_hid);
            if (devices) {
                printf("hid: %ld keyboard device(s)\n", (long)CFSetGetCount(devices));
                CFRelease(devices);
            } else {
                puts("hid: IOHIDManagerCopyDevices -> NULL");
            }
        }
        printf("hid: fallback console fd %d is %s\n", g_console_fd,
               isatty(g_console_fd) ? "a tty" : "not a tty");
        return 0;
    }

    // --render headless capture.
    if (render_path) return render_mode(render_path, (uint32_t)width, (uint32_t)height);

    // Normal boot.
    int present = present_enabled_flag(present_flag);
    int branch = resolve_branch(choice, present);
    if (dry_run)
        printf("chooser: dry-run -> %s\n", branch ? "shimmy" : "macOS");
    if (branch) branch_shimmy(dry_run);
    else branch_macos(kickstart_flag, dry_run);
    return 0;
}
