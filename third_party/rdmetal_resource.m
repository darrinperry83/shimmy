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
    id<MTLTexture> depthTexture;
    id<MTLCommandBuffer> pending;
    uint64_t sequence;
};

struct RDMetalDepth {
    RDMetalCore *core;
    id<MTLTexture> texture;
    id<MTLCommandBuffer> pending;
    uint32_t width,height;
    uint64_t sequence;
};

RDMetalDepth *rd_metal_depth_create(RDMetalCore *core,uint32_t width,uint32_t height) {
    if(!core||!width||!height||width>4096||height>4096||
       (uint64_t)width*height*8>128u*1024u*1024u)return NULL;
    MTLTextureDescriptor *desc=[MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8
        width:width height:height mipmapped:NO];
    desc.storageMode=MTLStorageModePrivate;
    desc.usage=MTLTextureUsageRenderTarget;
    id<MTLTexture> texture=[core->device newTextureWithDescriptor:desc];
    if(!texture)return NULL;
    RDMetalDepth *depth=calloc(1,sizeof *depth);
    if(!depth){[texture release];return NULL;}
    depth->core=core;depth->texture=texture;
    depth->width=width;depth->height=height;
    return depth;
}

void *rd_metal_depth_texture(RDMetalDepth *depth) {
    return depth?(__bridge void *)depth->texture:NULL;
}

uint64_t rd_metal_depth_clear(RDMetalDepth *depth,double value,
                              uint32_t stencil,uint32_t mask) {
    if(!depth||depth->pending||!isfinite(value)||value<0||value>1||
       stencil>255||!mask||(mask&~3u))return 0;
    id<MTLCommandBuffer> command=[depth->core->queue commandBuffer];
    if(!command)return 0;
    MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
    pass.depthAttachment.texture=depth->texture;
    pass.depthAttachment.loadAction=(mask&1u)?MTLLoadActionClear:MTLLoadActionLoad;
    pass.depthAttachment.storeAction=MTLStoreActionStore;
    pass.depthAttachment.clearDepth=value;
    pass.stencilAttachment.texture=depth->texture;
    pass.stencilAttachment.loadAction=(mask&2u)?MTLLoadActionClear:MTLLoadActionLoad;
    pass.stencilAttachment.storeAction=MTLStoreActionStore;
    pass.stencilAttachment.clearStencil=stencil;
    id<MTLRenderCommandEncoder> encoder=[command renderCommandEncoderWithDescriptor:pass];
    if(!encoder)return 0;
    [encoder endEncoding];
    depth->pending=[command retain];[command commit];
    return ++depth->sequence;
}

int rd_metal_depth_poll(RDMetalDepth *depth,uint64_t sequence) {
    if(!depth||!depth->pending||!sequence||sequence!=depth->sequence)return -1;
    MTLCommandBufferStatus status=depth->pending.status;
    if(status<MTLCommandBufferStatusCompleted&&status!=MTLCommandBufferStatusError)
        return 0;
    bool okay=status==MTLCommandBufferStatusCompleted;
    [depth->pending release];depth->pending=nil;
    return okay?1:-1;
}

void rd_metal_depth_destroy(RDMetalDepth *depth) {
    if(!depth)return;
    if(depth->pending){[depth->pending waitUntilCompleted];[depth->pending release];}
    [depth->texture release];free(depth);
}

static id<MTLTexture> image_depth_texture(RDMetalImage *image) {
    if (!image) return nil;
    if (!image->depthTexture) {
        MTLTextureDescriptor *desc = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8
            width:IOSurfaceGetWidth(image->surface)
            height:IOSurfaceGetHeight(image->surface) mipmapped:NO];
        desc.storageMode = MTLStorageModePrivate;
        desc.usage = MTLTextureUsageRenderTarget;
        image->depthTexture = [image->core->device newTextureWithDescriptor:desc];
    }
    return image->depthTexture;
}

void *rd_metal_image_depth_texture(RDMetalImage *image) {
    return (__bridge void *)image_depth_texture(image);
}

uint64_t rd_metal_image_clear_depth_stencil(RDMetalImage *image, double depth,
                                            uint32_t stencil, uint32_t mask) {
    if (!image || image->pending || !isfinite(depth) || depth < 0 || depth > 1 ||
        stencil > 255 || !mask || (mask & ~3u)) return 0;
    id<MTLTexture> texture = image_depth_texture(image);
    if (!texture) return 0;
    id<MTLCommandBuffer> command = [image->core->queue commandBuffer];
    if (!command) return 0;
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.depthAttachment.texture = texture;
    pass.depthAttachment.loadAction = (mask & 1u) ? MTLLoadActionClear : MTLLoadActionLoad;
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    pass.depthAttachment.clearDepth = depth;
    pass.stencilAttachment.texture = texture;
    pass.stencilAttachment.loadAction = (mask & 2u) ? MTLLoadActionClear : MTLLoadActionLoad;
    pass.stencilAttachment.storeAction = MTLStoreActionStore;
    pass.stencilAttachment.clearStencil = stencil;
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    if (!encoder) return 0;
    [encoder endEncoding];
    image->pending = [command retain];
    [command commit];
    return ++image->sequence;
}

uint64_t rd_metal_image_clear_depth(RDMetalImage *image, double depth,
                                    uint32_t stencil) {
    return rd_metal_image_clear_depth_stencil(image, depth, stencil, 3u);
}

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
    if (!core || !width || !height || width > 4096 || height > 4096 ||
        (uint64_t)width * height * 4 > 64u * 1024u * 1024u) return NULL;
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

RDMetalImage *rd_metal_image_import(RDMetalCore *core, IOSurfaceRef surface) {
    if (!core || !surface) return NULL;
    size_t width = IOSurfaceGetWidth(surface), height = IOSurfaceGetHeight(surface);
    size_t stride = IOSurfaceGetBytesPerRow(surface);
    if (!width || !height || width > 4096 || height > 4096 ||
        IOSurfaceGetPixelFormat(surface) != (uint32_t)'BGRA' ||
        stride < width * 4 || stride > 4096 * 4 ||
        (uint64_t)stride * height > 64u * 1024u * 1024u) return NULL;
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                        width:width height:height mipmapped:NO];
    descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [core->device newTextureWithDescriptor:descriptor iosurface:surface plane:0];
    if (!texture) return NULL;
    RDMetalImage *image = calloc(1, sizeof *image);
    if (!image) { [texture release]; return NULL; }
    image->core = core;
    image->surface = (IOSurfaceRef)CFRetain(surface);
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
    [image->depthTexture release];
    if (image->surface) CFRelease(image->surface);
    free(image);
}
