#ifndef RDMETAL_RESOURCE_H
#define RDMETAL_RESOURCE_H
#include <stdbool.h>
#include <stdint.h>
#include <IOSurface/IOSurface.h>

typedef struct RDMetalCore RDMetalCore;
typedef struct RDMetalImage RDMetalImage;

RDMetalCore *rd_metal_core_create(void);
void rd_metal_core_destroy(RDMetalCore *core);
RDMetalImage *rd_metal_image_create(RDMetalCore *core, uint32_t width, uint32_t height);
/* Host-only: wrap an already-authorized transferred IOSurface. Retains it. */
RDMetalImage *rd_metal_image_import(RDMetalCore *core, IOSurfaceRef surface);
uint64_t rd_metal_image_clear(RDMetalImage *image, float red, float green, float blue, float alpha);
/* Depth is host-owned and never exported as an IOSurface. The returned texture
 * is borrowed and remains valid until the image is destroyed. */
void *rd_metal_image_depth_texture(RDMetalImage *image);
uint64_t rd_metal_image_clear_depth(RDMetalImage *image, double depth, uint32_t stencil);
bool rd_metal_image_wait(RDMetalImage *image, uint64_t sequence);
int rd_metal_image_poll(RDMetalImage *image, uint64_t sequence); /* 0 pending, 1 complete, -1 failed/stale */
IOSurfaceRef rd_metal_image_surface(RDMetalImage *image); /* borrowed; image retains it */
void rd_metal_image_destroy(RDMetalImage *image);
#endif
