// rd_mac_app.m — a tiny "real mac application" whose window is a CAMetalLayer and
// whose live content (an animated Metal render) lives in a shared IOSurface.
//
// It publishes that IOSurface to the shim through the existing cross-process
// mach-port handoff (rdgpu_surface_handoff.c, keyed by a carrier file's fstat
// identity), so the shim can IOSurfaceLock/read the app's REAL pixels into its
// Hyprland-owned wl_shm surface. This is Stage 2 of the mac-app integration: a
// real mac app's Metal/IOSurface content wired into the Hyprland-owned surface.
//
// The app is launched by the shim (fork/exec, so it inherits $RDSOCKETGROUP and
// both sides derive the same handoff bootstrap name):
//
//   rd_mac_app --rdmac-carrier=<path> [--rdmac-w=<w> --rdmac-h=<h>]
//
// Build (also wired into ../build.sh via build_mac_app):
//   clang -arch arm64 -O2 -fobjc-arc -ObjC rd_mac_app.m rdgpu_surface_handoff.c \
//       -o rd_mac_app -I. -framework Cocoa -framework Metal -framework QuartzCore \
//       -framework IOSurface -framework CoreFoundation

#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <IOSurface/IOSurface.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rdgpu_surface_handoff.h"

// Fullscreen-triangle vertex shader (3 vertices -> one triangle covering NDC).
static const char *kVS =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VOut { float4 pos [[position]]; };\n"
    "vertex VOut rd_vtx(uint vid [[vertex_id]]) {\n"
    "    float2 p = (vid == 0) ? float2(-1,-1) : (vid == 1) ? float2(3,-1) : float2(-1,3);\n"
    "    VOut o; o.pos = float4(p, 0.0, 1.0); return o;\n"
    "}\n";

// Fragment shader: an animated hue gradient + a sweeping bright bar + a frame
// counter band (8 binary dots). Every pixel changes every frame, so a two-frame
// sample is unambiguous proof of LIVE content.
static const char *kFS =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VOut { float4 pos [[position]]; };\n"
    "fragment float4 rd_frag(VOut in [[stage_in]],\n"
    "                        constant float2& res  [[buffer(0)]],\n"
    "                        constant float&  time [[buffer(1)]],\n"
    "                        constant uint&   frame [[buffer(2)]]) {\n"
    "    float2 uv = in.pos.xy / res;\n"
    "    float t = time * 0.6;\n"
    "    float3 bg = 0.5 + 0.5 * cos(6.2831853 * (uv.x * 1.5 + t) + float3(0.0, 2.094, 4.188));\n"
    "    float barx = fmod(time * 0.45, 1.5) - 0.25;\n"
    "    float bar = smoothstep(0.0, 0.03, 1.0 - abs(uv.x - barx));\n"
    "    float3 col = mix(bg, float3(1.0), bar * 0.85);\n"
    "    if (uv.y > 0.90) {\n"
    "        float d = 0.0;\n"
    "        for (uint i = 0; i < 8; i++) {\n"
    "            float cx = 0.12 + 0.10 * float(i);\n"
    "            float dd = length(float2(uv.x - cx, uv.y - 0.95) * float2(9.0, 14.0));\n"
    "            uint bit = (frame >> (7 - i)) & 1u;\n"
    "            d += bit ? smoothstep(1.0, 0.55, dd) : 0.0;\n"
    "        }\n"
    "        col = mix(float3(0.06), float3(1.0, 0.9, 0.25), clamp(d, 0.0, 1.0));\n"
    "    }\n"
    "    return float4(col, 1.0);\n"
    "}\n";

static int g_w = 800, g_h = 600;
static const char *g_carrier = NULL;

static IOSurfaceRef make_iosurface(size_t w, size_t h) {
    size_t stride = IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, w * 4);
    CFNumberRef nw = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, (int[]){ (int)w });
    CFNumberRef nh = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, (int[]){ (int)h });
    CFNumberRef nb = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, (int[]){ 4 });
    CFNumberRef ns = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, (int[]){ (int)stride });
    CFNumberRef nf = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, (int[]){ 0x42475241 }); // 'BGRA'
    const void *keys[] = { kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement,
                           kIOSurfaceBytesPerRow, kIOSurfacePixelFormat };
    const void *vals[] = { nw, nh, nb, ns, nf };
    CFDictionaryRef d = CFDictionaryCreate(kCFAllocatorDefault, keys, vals, 5,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    IOSurfaceRef s = IOSurfaceCreate(d);
    CFRelease(nw); CFRelease(nh); CFRelease(nb); CFRelease(ns); CFRelease(nf); CFRelease(d);
    return s;
}

static int publish_surface(IOSurfaceRef surface, const char *carrier) {
    int fd = open(carrier, O_RDONLY);
    if (fd < 0) fd = open(carrier, O_CREAT | O_RDWR, 0600);
    if (fd < 0) { fprintf(stderr, "[rd-mac-app] carrier open: %s\n", strerror(errno)); return -1; }
    struct stat st;
    if (fstat(fd, &st)) { fprintf(stderr, "[rd-mac-app] carrier fstat: %s\n", strerror(errno)); close(fd); return -1; }
    int rc = rdgpu_handoff_publish(surface, st.st_dev, st.st_ino);
    fprintf(stderr, "[rd-mac-app] publish surface id=%u dev=%d ino=%llu rc=%d\n",
            IOSurfaceGetID(surface), (int)st.st_dev, (unsigned long long)st.st_ino, rc);
    close(fd);
    return rc;
}

@interface RdMacApp : NSObject
@property(nonatomic) id<MTLDevice> device;
@property(nonatomic) id<MTLCommandQueue> queue;
@property(nonatomic) id<MTLRenderPipelineState> pipe;
@property(nonatomic) IOSurfaceRef surface;
@property(nonatomic) id<MTLTexture> texture;
@property(nonatomic) CAMetalLayer *layer;
@property(nonatomic) uint64_t frame;
@property(nonatomic) double t0;
@end

@implementation RdMacApp

- (void)render {
    if (!self.surface || !self.texture) return;
    self.frame++;

    id<MTLCommandBuffer> cb = [self.queue commandBuffer];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = self.texture;
    rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:self.pipe];
    float res[2] = { (float)IOSurfaceGetWidth(self.surface), (float)IOSurfaceGetHeight(self.surface) };
    float time = (float)(CFAbsoluteTimeGetCurrent() - self.t0);
    uint32_t frame = (uint32_t)(self.frame & 0xFF);
    [enc setFragmentBytes:res length:sizeof(res) atIndex:0];
    [enc setFragmentBytes:&time length:sizeof(time) atIndex:1];
    [enc setFragmentBytes:&frame length:sizeof(frame) atIndex:2];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];

    // Mirror the SAME pixels into the on-screen CAMetalLayer window (a real mac
    // window), proving the window and the shared IOSurface carry identical content.
    id<CAMetalDrawable> drawable = [self.layer nextDrawable];
    if (drawable) {
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromTexture:self.texture sourceSlice:0 sourceLevel:0
            sourceOrigin:MTLOriginMake(0, 0, 0)
            sourceSize:MTLSizeMake(IOSurfaceGetWidth(self.surface), IOSurfaceGetHeight(self.surface), 1)
            toTexture:drawable.texture destinationSlice:0 destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        [cb presentDrawable:drawable];
    }
    [cb commit];
}

@end

int main(int argc, char **argv) {
    @autoreleasepool {
        for (int i = 1; i < argc; i++) {
            if (!strncmp(argv[i], "--rdmac-carrier=", 16)) g_carrier = argv[i] + 16;
            else if (!strncmp(argv[i], "--rdmac-w=", 10)) { int v = atoi(argv[i] + 10); if (v > 0 && v <= 8192) g_w = v; }
            else if (!strncmp(argv[i], "--rdmac-h=", 10)) { int v = atoi(argv[i] + 10); if (v > 0 && v <= 8192) g_h = v; }
        }
        if (!g_carrier) { fprintf(stderr, "[rd-mac-app] missing --rdmac-carrier\n"); return 2; }

        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) { fprintf(stderr, "[rd-mac-app] no Metal device\n"); return 2; }
        id<MTLCommandQueue> queue = [device newCommandQueue];

        IOSurfaceRef surface = make_iosurface((size_t)g_w, (size_t)g_h);
        if (!surface) { fprintf(stderr, "[rd-mac-app] IOSurfaceCreate failed\n"); return 2; }
        fprintf(stderr, "[rd-mac-app] surface id=%u %dx%d stride=%zu fmt='%c%c%c%c'\n",
                IOSurfaceGetID(surface), g_w, g_h, IOSurfaceGetBytesPerRow(surface),
                (int)((IOSurfaceGetPixelFormat(surface) >> 24) & 0xff),
                (int)((IOSurfaceGetPixelFormat(surface) >> 16) & 0xff),
                (int)((IOSurfaceGetPixelFormat(surface) >> 8) & 0xff),
                (int)(IOSurfaceGetPixelFormat(surface) & 0xff));

        MTLTextureDescriptor *td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
            width:(NSUInteger)g_w height:(NSUInteger)g_h mipmapped:NO];
        td.storageMode = MTLStorageModeShared;
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        id<MTLTexture> texture = [device newTextureWithDescriptor:td iosurface:surface plane:0];
        if (!texture) { fprintf(stderr, "[rd-mac-app] newTextureWithDescriptor(iosurface) failed\n"); return 2; }

        NSError *err = nil;
        id<MTLLibrary> lib = [device newLibraryWithSource:@(kVS) options:nil error:&err];
        if (!lib) { fprintf(stderr, "[rd-mac-app] vs lib: %s\n", err.localizedDescription.UTF8String ?: ""); return 2; }
        id<MTLFunction> vf = [lib newFunctionWithName:@"rd_vtx"];
        id<MTLLibrary> flib = [device newLibraryWithSource:@(kFS) options:nil error:&err];
        if (!flib) { fprintf(stderr, "[rd-mac-app] fs lib: %s\n", err.localizedDescription.UTF8String ?: ""); return 2; }
        id<MTLFunction> ff = [flib newFunctionWithName:@"rd_frag"];
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = vf;
        pd.fragmentFunction = ff;
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        id<MTLRenderPipelineState> pipe = [device newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!pipe) { fprintf(stderr, "[rd-mac-app] pipeline: %s\n", err.localizedDescription.UTF8String ?: ""); return 2; }

        RdMacApp *app = [RdMacApp new];
        app.device = device; app.queue = queue; app.pipe = pipe;
        app.surface = surface; app.texture = texture;
        app.t0 = CFAbsoluteTimeGetCurrent();

        // Publish AFTER the first render so the surface already carries content.
        [app render];
        if (publish_surface(surface, g_carrier) != 0) return 2;

        // A real mac window (CAMetalLayer) showing the same content.
        NSApplication *nsapp = [NSApplication sharedApplication];
        [nsapp setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSWindow *win = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(120, 120, g_w, g_h)
            styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable)
            backing:NSBackingStoreBuffered defer:NO];
        win.title = @"rd-mac-app (shared IOSurface)";
        win.releasedWhenClosed = NO;
        CAMetalLayer *layer = [CAMetalLayer layer];
        layer.device = device;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = YES;
        layer.drawableSize = CGSizeMake(g_w, g_h);
        win.contentView.layer = layer;
        win.contentView.wantsLayer = YES;
        app.layer = layer;
        [win makeKeyAndOrderFront:nil];
        [nsapp activateIgnoringOtherApps:YES];

        // Render continuously so the shim sees live, changing content.
        NSTimer *timer = [NSTimer timerWithTimeInterval:1.0 / 30.0 repeats:YES block:^(NSTimer *t) {
            (void)t;
            [app render];
        }];
        [[NSRunLoop mainRunLoop] addTimer:timer forMode:NSDefaultRunLoopMode];

        fprintf(stderr, "[rd-mac-app] running: window '%s' + shared IOSurface id=%u\n",
                win.title.UTF8String ?: "", IOSurfaceGetID(surface));

        [nsapp run];

        [timer invalidate];
        CFRelease(surface);
        return 0;
    }
}
