// rdgpu_native_output.m — native display geometry for the Wayland output advertisement.
//
// Auto-detects the physical pixel size and backing scale of the display the guest output maps
// to, so wlsrv.c can advertise a real native mode (and the xdg_toplevel configure can default
// to it) instead of a hardcoded 3440x1440@1. No RDNATIVE_FIT_OUTPUT_* env is required.
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#include <math.h>
#include "rdgpu_native_output.h"

// window.m owns the primary guest window (gWin) and publishes this getter. Reading NSScreen
// geometry is thread-safe (it is backed by CGDisplay queries), and gWin is a write-once pointer,
// so this is safe to call from the compositor's wl_thread while [NSApp run] owns the main thread.
extern NSScreen* rdgpu_primary_screen(void);

static NSScreen* output_screen(size_t index) {
    if (index == 0) {
        NSScreen* s = rdgpu_primary_screen();
        return s ?: NSScreen.mainScreen;
    }
    // Additional outputs: enumerate NSScreen.screens in order, skipping the primary screen.
    NSScreen* primary = rdgpu_primary_screen();
    if (!primary) primary = NSScreen.mainScreen;
    size_t i = 0;
    for (NSScreen* s in NSScreen.screens) {
        if (s == primary) continue;
        if (i == index - 1) return s;
        i++;
    }
    return nil;
}

int rdgpu_native_output(size_t index, uint32_t* w, uint32_t* h, double* scale) {
    NSScreen* screen = output_screen(index);
    if (!screen) {
        if (w) *w = 0;
        if (h) *h = 0;
        if (scale) *scale = 1.0;
        return -1;
    }
    NSRect frame = screen.frame;
    double bs = (double)screen.backingScaleFactor;
    if (bs < 0.5) bs = 1.0;                       // never advertise a nonsense factor
    if (w) *w = (uint32_t)lround(frame.size.width  * bs);
    if (h) *h = (uint32_t)lround(frame.size.height * bs);
    if (scale) *scale = bs;
    return 0;
}

int rdgpu_drm_connector_output(uint32_t connector_id, uint32_t* w, uint32_t* h) {
    if (connector_id != 1 && connector_id != 2) return -1;
    BOOL wantBuiltin = connector_id == 2;
    for (NSScreen* screen in NSScreen.screens) {
        NSNumber* number = screen.deviceDescription[@"NSScreenNumber"];
        if (!number) continue;
        CGDirectDisplayID did = (CGDirectDisplayID)number.unsignedIntValue;
        if (!CGDisplayIsActive(did) || (BOOL)CGDisplayIsBuiltin(did) != wantBuiltin)
            continue;
        NSRect frame = screen.frame;
        double scale = (double)screen.backingScaleFactor;
        if (scale < 0.5) return -1;
        uint32_t width = (uint32_t)lround(frame.size.width * scale);
        uint32_t height = (uint32_t)lround(frame.size.height * scale);
        if (width < 1 || height < 1 || width > UINT16_MAX || height > UINT16_MAX)
            return -1;
        if (w) *w = width;
        if (h) *h = height;
        return 0;
    }
    return -1;
}
