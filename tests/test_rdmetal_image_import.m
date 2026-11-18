#import "../third_party/rdmetal_resource.h"
#import "../third_party/rdgpu_broker.h"
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

    RDGPUSession *broker = rdgpu_local_session_create();
    assert(broker);
    uint8_t lease[16] = {0};
    uint64_t generation = 0;
    assert(rdgpu_local_import_surface(broker, surface, lease, &generation));
    uint32_t width = 0, height = 0, stride = 0;
    assert(rdgpu_local_image_geometry(broker, lease, generation,
                                      &width, &height, &stride));
    assert(width == 16 && height == 16 && stride >= 64);
    IOSurfaceRef broker_surface = rdgpu_local_draw_surface(broker, lease, generation);
    assert(broker_surface && IOSurfaceGetID(broker_surface) == surface_id);
    CFRelease(broker_surface);
    rdgpu_session_disconnect(broker);

    rd_metal_image_destroy(producer);
    assert(IOSurfaceGetID(rd_metal_image_surface(consumer)) == surface_id);
    rd_metal_image_destroy(consumer);
    rd_metal_core_destroy(core);
    puts("imported broker lease shares the producer IOSurface");
    return 0;
}
