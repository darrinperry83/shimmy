#import "../third_party/rdmetal_resource.h"
#import "../third_party/rdgpu_broker.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

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

    RDGPUSession *session = rdgpu_local_session_create();
    assert(session);
    RDGPURequest request = {.magic=RDGPU_WIRE_MAGIC,
                            .version=RDGPU_WIRE_VERSION,
                            .op=RDGPU_CREATE_BGRA8,
                            .bytes=RDGPU_WIRE_BYTES,
                            .request_id=1,
                            .arg0=16u | (16ull << 32)};
    RDGPUReply created = rdgpu_dispatch(session, &request);
    assert(created.status == RDGPU_OK && created.value1);
    memcpy(request.lease, created.lease, sizeof request.lease);
    request.arg1 = created.value1;
    request.op = RDGPU_CLEAR_DEPTH_STENCIL;
    request.request_id++;
    float value = 0.25f;
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    request.arg0 = bits | (7ull << 32) | (3ull << 40);
    RDGPUReply cleared = rdgpu_dispatch(session, &request);
    assert(cleared.status == RDGPU_OK && cleared.value0);
    request.request_id++;
    assert(rdgpu_dispatch(session, &request).status == RDGPU_BAD_STATE);
    request.op = RDGPU_WAIT;
    request.arg0 = cleared.value0;
    RDGPUReply waited;
    do {
        request.request_id++;
        waited = rdgpu_dispatch(session, &request);
        if (waited.status == RDGPU_PENDING) usleep(1000);
    } while (waited.status == RDGPU_PENDING);
    assert(waited.status == RDGPU_OK);
    request.op = RDGPU_CLEAR_DEPTH_STENCIL;
    request.request_id++;
    request.arg0 = bits | (1ull << 42);
    assert(rdgpu_dispatch(session, &request).status == RDGPU_BAD_MESSAGE);
    request.request_id++;
    request.arg0 = bits;
    assert(rdgpu_dispatch(session, &request).status == RDGPU_BAD_MESSAGE);
    rdgpu_session_disconnect(session);
    rd_metal_image_destroy(image);
    rd_metal_core_destroy(core);
    puts("Metal depth attachment clears and fences complete");
    return 0;
}
