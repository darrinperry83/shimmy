// mac_surface_host.m — host-side (macOS) helpers for the mac-app Wayland surface.
//
// Compiled into shim_gui. Exposes C entry points used by wlsrv.c's mac_surface_*
// region:
//   * rd_mac_launch_bundle()  — NSWorkspace launch of a mac app (.app bundle /
//     bundle-id / name), returning its pid + display name. This is the "launcher"
//     half of the mac-app integration.
//   * rd_mac_capture_window() — best-effort CGWindowListCreateImage capture of a
//     launched app's window (BGRA, malloc'd). Needs Screen Recording TCC for
//     FOREIGN apps; returns NULL (with a logged reason) when the permission is
//     missing, in which case the surface falls back to the IOSurface test app or
//     the solid placeholder.
//
// The REAL pixel path (no TCC, no sudo) is the IOSurface test app
// (rd_mac_app.m) published through rdgpu_surface_handoff; see wlsrv.c.

#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <CoreServices/CoreServices.h>
#import <CoreGraphics/CoreGraphics.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

static NSURL *rd_mac_resolve_app_url(NSString *spec) {
    // Absolute/relative path (ends in .app or contains a slash).
    if ([spec containsString:@"/"] || [spec hasSuffix:@".app"]) {
        NSString *p = [spec stringByStandardizingPath];
        if ([[NSFileManager defaultManager] fileExistsAtPath:p])
            return [NSURL fileURLWithPath:p];
    }
    // Bundle identifier, e.g. com.apple.calculator.
    NSURL *u = [[NSWorkspace sharedWorkspace] URLForApplicationWithBundleIdentifier:spec];
    if (u) return u;
    // LaunchServices fallback (bundle id or name in a non-standard location).
    CFURLRef url = NULL;
    OSStatus st = LSFindApplicationForInfo(kLSUnknownCreator, (__bridge CFStringRef)spec,
                                           NULL, NULL, &url);
    if (st == noErr && url) return (__bridge_transfer NSURL *)url;
    if (url) CFRelease(url);
    // Name lookup (e.g. "Calculator").
    NSString *p = [[NSWorkspace sharedWorkspace] fullPathForApplication:spec];
    if (p) return [NSURL fileURLWithPath:p];
    return nil;
}

// Launch a mac app by bundle-id / name / path via NSWorkspace.
// Returns the pid (>0), or 0 on failure (not found / launch error). Fills
// out_name with the app's display name (CFBundleName) when non-NULL.
int rd_mac_launch_bundle(const char *spec, char *out_name, size_t onsz) {
    @autoreleasepool {
        if (!spec || !*spec) return 0;
        NSString *s = [NSString stringWithUTF8String:spec];
        NSURL *url = rd_mac_resolve_app_url(s);
        if (!url) {
            fprintf(stderr, "[mac-launch] no app matches \"%s\"\n", spec);
            return 0;
        }
        NSBundle *b = [NSBundle bundleWithURL:url];
        NSString *name = [b objectForInfoDictionaryKey:@"CFBundleName"]
                      ?: [[url lastPathComponent] stringByDeletingPathExtension]
                      ?: s;
        if (out_name && onsz) snprintf(out_name, onsz, "%s", name.UTF8String ?: spec);

        NSWorkspaceOpenConfiguration *cfg = [NSWorkspaceOpenConfiguration configuration];
        __block pid_t pid = -1;
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        [[NSWorkspace sharedWorkspace] openApplicationAtURL:url configuration:cfg
            completionHandler:^(NSRunningApplication *app, NSError *err) {
                if (err) fprintf(stderr, "[mac-launch] openApplicationAtURL error: %s\n",
                                 err.localizedDescription.UTF8String ?: "");
                pid = app ? app.processIdentifier : -1;
                dispatch_semaphore_signal(done);
            }];
        dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC));
        fprintf(stderr, "[mac-launch] launched \"%s\" (bundle %s) pid=%d\n",
                name.UTF8String ?: "", url.path.UTF8String ?: "", pid);
        return pid > 0 ? pid : 0;
    }
}

// Best-effort capture of the first on-screen window owned by `pid`.
// Returns a malloc'd BGRA buffer (caller frees), writing *out_w/*out_h/*out_stride.
// Requires Screen Recording TCC for a FOREIGN app's window; returns NULL when the
// window list is unavailable, the app has no on-screen window, or capture is denied.
void *rd_mac_capture_window(pid_t pid, int *out_w, int *out_h, int *out_stride) {
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (out_stride) *out_stride = 0;
    if (pid <= 0) return NULL;

    static int tcc_logged;
    if (!tcc_logged) {
        tcc_logged = 1;
        fprintf(stderr, "[mac-capture] CGPreflightScreenCaptureAccess = %s\n",
                CGPreflightScreenCaptureAccess() ? "GRANTED" : "NOT granted");
    }

    CFArrayRef wins = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
    if (!wins) return NULL;

    CGWindowID target = 0;
    int tw = 0, th = 0;
    CFIndex n = CFArrayGetCount(wins);
    for (CFIndex i = 0; i < n; i++) {
        CFDictionaryRef d = (CFDictionaryRef)CFArrayGetValueAtIndex(wins, i);
        CFNumberRef opid = CFDictionaryGetValue(d, kCGWindowOwnerPID);
        long long op = 0;
        if (opid) CFNumberGetValue(opid, kCFNumberLongLongType, &op);
        if (op != (long long)pid) continue;
        // Pick the largest on-screen window for the app (its main window).
        CFDictionaryRef bnd = CFDictionaryGetValue(d, kCGWindowBounds);
        int w = 0, h = 0;
        if (bnd) {
            CFNumberRef bw = CFDictionaryGetValue(bnd, CFSTR("Width"));
            CFNumberRef bh = CFDictionaryGetValue(bnd, CFSTR("Height"));
            long long lw = 0, lh = 0;
            if (bw) CFNumberGetValue(bw, kCFNumberLongLongType, &lw);
            if (bh) CFNumberGetValue(bh, kCFNumberLongLongType, &lh);
            w = (int)lw; h = (int)lh;
        }
        CFNumberRef wn = CFDictionaryGetValue(d, kCGWindowNumber);
        long long cw = 0;
        if (wn) CFNumberGetValue(wn, kCFNumberLongLongType, &cw);
        if (cw > 0 && w * h > tw * th) { target = (CGWindowID)cw; tw = w; th = h; }
    }
    CFRelease(wins);
    if (!target) {
        fprintf(stderr, "[mac-capture] pid=%d has no on-screen window yet\n", pid);
        return NULL;
    }

    // CGWindowListCreateImage is obsoleted (marked unavailable) on macOS 15+ but the
    // symbol is still present; resolve it at runtime so this helper compiles on any
    // SDK and works where Screen Recording TCC has been granted.
    typedef CGImageRef (*CGCreateImageFn)(CGRect, CGWindowListOption, CGWindowID, CGWindowImageOption);
    static CGCreateImageFn createImage;
    if (!createImage) createImage = (CGCreateImageFn)dlsym(RTLD_DEFAULT, "CGWindowListCreateImage");
    if (!createImage) {
        fprintf(stderr, "[mac-capture] CGWindowListCreateImage unavailable on this OS\n");
        return NULL;
    }
    CGImageRef img = createImage(CGRectNull, kCGWindowListOptionIncludingWindow,
                                 target, kCGWindowImageBoundsIgnoreFraming);
    if (!img) {
        fprintf(stderr, "[mac-capture] CGWindowListCreateImage(window %u) failed "
                        "(Screen Recording TCC required for foreign apps)\n", target);
        return NULL;
    }
    size_t iw = CGImageGetWidth(img), ih = CGImageGetHeight(img);
    size_t stride = iw * 4;
    uint8_t *buf = malloc(stride * ih);
    if (!buf) { CGImageRelease(img); return NULL; }
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(buf, iw, ih, 8, stride, cs,
        kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst);
    CGColorSpaceRelease(cs);
    if (!ctx) { free(buf); CGImageRelease(img); return NULL; }
    CGContextDrawImage(ctx, CGRectMake(0, 0, iw, ih), img);
    CGContextRelease(ctx);
    CGImageRelease(img);
    if (out_w) *out_w = (int)iw;
    if (out_h) *out_h = (int)ih;
    if (out_stride) *out_stride = (int)stride;
    fprintf(stderr, "[mac-capture] captured window %u pid=%d %zux%zu\n", target, pid, iw, ih);
    return buf;
}

#pragma clang diagnostic pop
