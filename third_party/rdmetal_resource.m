#import "rdmetal_resource.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <math.h>
#include <stdlib.h>

struct RDMetalCore {
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
};

struct RDMetalImage {
    RDMetalCore *core;
    IOSurfaceRef surface;
    id<MTLTexture> texture;
    id<MTLCommandBuffer> pending;
    uint64_t sequence;
};

RDMetalCore *rd_metal_core_create(void) {
    RDMetalCore *core = calloc(1, sizeof *core);
    if (!core) return NULL;
    core->device = MTLCreateSystemDefaultDevice();
    core->queue = [core->device newCommandQueue];
    if (!core->device || !core->queue) {
        [core->queue release]; [core->device release]; free(core); return NULL;
    }
    return core;
}

void rd_metal_core_destroy(RDMetalCore *core) {
    if (!core) return;
    [core->queue release]; [core->device release]; free(core);
}

RDMetalImage *rd_metal_image_create(RDMetalCore *core, uint32_t width, uint32_t height) {
    if (!core || !width || !height || width > 2048 || height > 2048 ||
        (uint64_t)width * height * 4 > 16u * 1024u * 1024u) return NULL;
    NSDictionary *properties = @{
        (__bridge NSString *)kIOSurfaceWidth: @(width),
        (__bridge NSString *)kIOSurfaceHeight: @(height),
        (__bridge NSString *)kIOSurfaceBytesPerElement: @4,
        (__bridge NSString *)kIOSurfacePixelFormat: @((uint32_t)'BGRA'),
    };
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)properties);
    if (!surface) return NULL;
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                        width:width height:height mipmapped:NO];
    descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [core->device newTextureWithDescriptor:descriptor iosurface:surface plane:0];
    if (!texture) { CFRelease(surface); return NULL; }
    RDMetalImage *image = calloc(1, sizeof *image);
    if (!image) { [texture release]; CFRelease(surface); return NULL; }
    image->core = core;
    image->surface = surface;
    image->texture = texture;
    return image;
}

uint64_t rd_metal_image_clear(RDMetalImage *image, float red, float green, float blue, float alpha) {
    if (!image || image->pending || !isfinite(red) || !isfinite(green) ||
        !isfinite(blue) || !isfinite(alpha) || red < 0 || red > 1 || green < 0 || green > 1 ||
        blue < 0 || blue > 1 || alpha < 0 || alpha > 1) return 0;
    id<MTLCommandBuffer> command = [image->core->queue commandBuffer];
    if (!command) return 0;
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = image->texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(red, green, blue, alpha);
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    if (!encoder) return 0;
    [encoder endEncoding];
    image->pending = [command retain];
    [command commit];
    return ++image->sequence;
}

bool rd_metal_image_wait(RDMetalImage *image, uint64_t sequence) {
    if (!image || !image->pending || !sequence || sequence != image->sequence) return false;
    [image->pending waitUntilCompleted];
    bool okay = image->pending.status == MTLCommandBufferStatusCompleted;
    [image->pending release];
    image->pending = nil;
    return okay;
}

int rd_metal_image_poll(RDMetalImage *image, uint64_t sequence) {
    if (!image || !image->pending || !sequence || sequence != image->sequence) return -1;
    MTLCommandBufferStatus status = image->pending.status;
    if (status < MTLCommandBufferStatusCompleted && status != MTLCommandBufferStatusError) return 0;
    bool okay = status == MTLCommandBufferStatusCompleted;
    [image->pending release]; image->pending = nil;
    return okay ? 1 : -1;
}

IOSurfaceRef rd_metal_image_surface(RDMetalImage *image) {
    return image ? image->surface : NULL;
}

void rd_metal_image_destroy(RDMetalImage *image) {
    if (!image) return;
    if (image->pending) { [image->pending waitUntilCompleted]; [image->pending release]; }
    [image->texture release];
    if (image->surface) CFRelease(image->surface);
    free(image);
}
