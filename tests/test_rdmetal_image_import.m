#import "../third_party/rdmetal_resource.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    RDMetalCore *core = rd_metal_core_create();
    assert(core);
    RDMetalImage *producer = rd_metal_image_create(core, 16, 16);
    assert(producer);
    IOSurfaceRef surface = rd_metal_image_surface(producer);
    assert(surface);
    uint32_t surface_id = IOSurfaceGetID(surface);
    RDMetalImage *consumer = rd_metal_image_import(core, surface);
    assert(consumer && rd_metal_image_surface(consumer));
    assert(IOSurfaceGetID(rd_metal_image_surface(consumer)) == surface_id);

    /* The imported texture must render into the producer's original backing. */
    uint64_t fence = rd_metal_image_clear(consumer, 1, 0, 0, 1);
    assert(fence && rd_metal_image_wait(consumer, fence));
    assert(IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL) == kIOReturnSuccess);
    const unsigned char *pixels = IOSurfaceGetBaseAddress(surface);
    assert(pixels && pixels[0] == 0 && pixels[1] == 0 &&
           pixels[2] == 255 && pixels[3] == 255);
    assert(IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL) == kIOReturnSuccess);

    rd_metal_image_destroy(producer);
    assert(IOSurfaceGetID(rd_metal_image_surface(consumer)) == surface_id);
    rd_metal_image_destroy(consumer);
    rd_metal_core_destroy(core);
    puts("imported Metal texture shares the producer IOSurface");
    return 0;
}
