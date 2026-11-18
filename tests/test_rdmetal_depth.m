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

    RDMetalDepth *shared=rd_metal_depth_create(core,16,16);
    RDMetalDepth *other=rd_metal_depth_create(core,16,16);
    assert(shared && other);
    assert(rd_metal_depth_texture(shared) &&
           rd_metal_depth_texture(shared)!=rd_metal_depth_texture(other));
    assert(!rd_metal_depth_clear(shared,0.5,0,0));
    uint64_t depth_fence=rd_metal_depth_clear(shared,0.25,7,3);
    assert(depth_fence && !rd_metal_depth_clear(shared,0.75,0,1));
    int depth_result;
    do {
        depth_result=rd_metal_depth_poll(shared,depth_fence);
        if(depth_result==0)usleep(1000);
    } while(depth_result==0);
    assert(depth_result==1);
    assert(rd_metal_depth_clear(other,1.0,0,1));
    rd_metal_depth_destroy(other);
    rd_metal_depth_destroy(shared);
    RDMetalDepth *greeter_size=rd_metal_depth_create(core,3600,2338);
    assert(greeter_size && rd_metal_depth_texture(greeter_size));
    rd_metal_depth_destroy(greeter_size);

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
    request.request_id++;
    request.op=RDGPU_CREATE_DEPTH;
    request.arg0=16u | (16ull<<32);
    request.arg1=0;
    memset(request.lease,0,sizeof request.lease);
    RDGPUReply depth_created=rdgpu_dispatch(session,&request);
    assert(depth_created.status==RDGPU_OK && depth_created.value1);
    request.request_id++;
    request.op=RDGPU_CLEAR_DEPTH_STENCIL;
    request.arg0=bits | (7ull<<32) | (3ull<<40);
    request.arg1=depth_created.value1;
    memcpy(request.lease,depth_created.lease,16);
    RDGPUReply depth_cleared=rdgpu_dispatch(session,&request);
    assert(depth_cleared.status==RDGPU_OK && depth_cleared.value0);
    request.op=RDGPU_WAIT;
    request.arg0=depth_cleared.value0;
    do {
        request.request_id++;
        waited=rdgpu_dispatch(session,&request);
        if(waited.status==RDGPU_PENDING)usleep(1000);
    } while(waited.status==RDGPU_PENDING);
    assert(waited.status==RDGPU_OK);
    request.request_id++;
    request.op=RDGPU_PREPARE_EXPORT;
    request.arg0=1;
    assert(rdgpu_dispatch(session,&request).status==RDGPU_DENIED);
    request.request_id++;
    request.op=RDGPU_RELEASE;
    request.arg0=0;
    assert(rdgpu_dispatch(session,&request).status==RDGPU_OK);
    rdgpu_session_disconnect(session);
    rd_metal_image_destroy(image);
    rd_metal_core_destroy(core);
    puts("Metal depth attachment clears and fences complete");
    return 0;
}
