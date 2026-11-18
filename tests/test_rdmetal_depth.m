#import "../third_party/rdmetal_resource.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void) {
    RDMetalCore *core = rd_metal_core_create();
    assert(core);
    RDMetalImage *image = rd_metal_image_create(core, 16, 16);
    assert(image);
    void *depth = rd_metal_image_depth_texture(image);
    assert(depth && depth == rd_metal_image_depth_texture(image));
    assert(!rd_metal_image_clear_depth(image, -0.1, 0));
    assert(!rd_metal_image_clear_depth(image, 0.5, 256));
    uint64_t first = rd_metal_image_clear_depth(image, 0.25, 7);
    assert(first && !rd_metal_image_clear_depth(image, 0.75, 0));
    assert(rd_metal_image_wait(image, first));
    uint64_t second = rd_metal_image_clear_depth(image, 0.75, 0);
    assert(second > first && rd_metal_image_wait(image, second));
    rd_metal_image_destroy(image);
    rd_metal_core_destroy(core);
    puts("Metal depth attachment clears and fences complete");
    return 0;
}
