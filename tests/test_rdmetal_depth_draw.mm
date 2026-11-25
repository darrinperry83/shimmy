#import "../third_party/rdmetal_resource.h"
#import "../third_party/rdgpu_draw_host.h"
#include "../third_party/rdmetal_draw_wire.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
#include <unistd.h>

static IOSurfaceRef target;
static IOSurfaceRef resolve(uint64_t handle, void *) {
    return handle == 1 ? (IOSurfaceRef)CFRetain(target) : nullptr;
}
static std::vector<uint32_t> shader(const char *file) {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    assert(in.good()); size_t size = (size_t)in.tellg();
    assert(size && size % 4 == 0); in.seekg(0);
    std::vector<uint32_t> words(size / 4);
    assert(in.read((char *)words.data(), size)); return words;
}
static std::vector<uint8_t> packet(const std::vector<uint32_t> &vs,
                                  const std::vector<uint32_t> &fs, float z) {
    struct Vertex { float x, y, z, u, v; } vertices[] = {
        {-1,-1,z,0,0}, {1,-1,z,1,0}, {0,1,z,.5,1}
    };
    rdmetal_draw_header_v7 state = {};
    auto &h = state.base.base.base.base.base.base;
    h.magic=RDMETAL_DRAW_MAGIC; h.version=RDMETAL_DRAW_VERSION_V7;
    h.total_bytes=sizeof state + 4*(vs.size()+fs.size()) + sizeof vertices;
    h.target_handle=1; h.target_width=h.target_height=16;
    h.vertex_count=3; h.vertex_stride=sizeof(Vertex);
    h.attribute_format=RDMETAL_DRAW_ATTR_FLOAT3;
    h.vertex_shader_words=(uint32_t)vs.size(); h.fragment_shader_words=(uint32_t)fs.size();
    h.vertex_bytes=sizeof vertices;
    auto &v3 = state.base.base.base.base;
    v3.attr1_format=RDMETAL_DRAW_ATTR_FLOAT2; v3.attr1_offset=12;
    state.base.base.base.scissor_width=state.base.base.base.scissor_height=16;
    state.base.base.depth_handle=2; state.base.base.depth_flags=3;
    state.base.base.depth_compare=1; // less
    state.color_write_mask=15; state.front_ccw=1;
    std::vector<uint8_t> data(h.total_bytes);
    uint8_t *out=data.data(); memcpy(out,&state,sizeof state); out+=sizeof state;
    memcpy(out,vs.data(),vs.size()*4); out+=vs.size()*4;
    memcpy(out,fs.data(),fs.size()*4); out+=fs.size()*4;
    memcpy(out,vertices,sizeof vertices); return data;
}
static int draw(std::vector<uint8_t> &data, RDMetalDepth *depth) {
    return rdgpu_draw_execute_with_depth(data.data(),data.size(),resolve,nullptr,
        nullptr,nullptr,nullptr,depth ? rd_metal_depth_texture(depth) : nullptr);
}
static void clear_depth(RDMetalDepth *depth, double value, unsigned stencil, unsigned mask) {
    uint64_t fence=rd_metal_depth_clear(depth,value,stencil,mask); assert(fence);
    int status=0;
    for(unsigned i=0;i<2000 && !status;i++) {
        status=rd_metal_depth_poll(depth,fence); if(!status) usleep(1000);
    }
    assert(status==1);
}
static void pixel(bool red, bool green) {
    assert(IOSurfaceLock(target,kIOSurfaceLockReadOnly,nullptr)==kIOReturnSuccess);
    const uint8_t *p=(const uint8_t *)IOSurfaceGetBaseAddress(target)+
        8*IOSurfaceGetBytesPerRow(target)+8*4;
    if((p[2]>240)!=red || (p[1]>240)!=green || p[0]>10) {
        fprintf(stderr,"unexpected center BGRA=%u,%u,%u,%u\n",p[0],p[1],p[2],p[3]);
        assert(false);
    }
    IOSurfaceUnlock(target,kIOSurfaceLockReadOnly,nullptr);
}
static rdmetal_draw_header_v7 &state(std::vector<uint8_t> &p) {
    return *(rdmetal_draw_header_v7 *)p.data();
}
int main() {
    auto vs=shader("tests/fixtures/depth-stencil/vertex.spv");
    auto red=shader("tests/fixtures/depth-stencil/red.spv");
    auto green=shader("tests/fixtures/depth-stencil/green.spv");
    RDMetalCore *core=rd_metal_core_create(); assert(core);
    RDMetalImage *image=rd_metal_image_create(core,16,16); assert(image);
    target=rd_metal_image_surface(image); assert(target);
    RDMetalDepth *depth=rd_metal_depth_create(core,16,16); assert(depth);
    uint64_t color=rd_metal_image_clear(image,0,0,0,1);
    assert(color && rd_metal_image_wait(image,color));
    clear_depth(depth,1,0,3);
    auto near=packet(vs,red,.25f), far=packet(vs,green,.75f);
    assert(draw(near,depth)==RDGPU_DRAW_OK); pixel(true,false);
    assert(draw(far,depth)==RDGPU_DRAW_OK); pixel(true,false); // occluded
    state(far).base.base.depth_flags=0;
    assert(draw(far,depth)==RDGPU_DRAW_OK); pixel(false,true); // depth test disabled

    clear_depth(depth,1,0,3);
    auto mask=packet(vs,red,.25f);
    state(mask).base.base.depth_flags=0; state(mask).color_write_mask=0;
    for(auto &face:state(mask).stencil) face={1,7,0,0,2,255,15,0xa3};
    assert(draw(mask,depth)==RDGPU_DRAW_OK); pixel(false,true); // stencil-only
    auto clipped=packet(vs,red,.25f);
    state(clipped).base.base.depth_flags=0;
    for(auto &face:state(clipped).stencil) face={1,2,0,0,0,15,0,3};
    assert(draw(clipped,depth)==RDGPU_DRAW_OK); pixel(true,false);
    auto rejected=packet(vs,green,.25f);
    state(rejected).base.base.depth_flags=0;
    for(auto &face:state(rejected).stencil) face={1,2,0,0,0,15,0,4};
    assert(draw(rejected,depth)==RDGPU_DRAW_OK); pixel(true,false); // masked compare fails
    clear_depth(depth,1,0,2); // stencil-only clear preserves color
    assert(draw(clipped,depth)==RDGPU_DRAW_OK); pixel(true,false);
    for(auto &face:state(rejected).stencil) face.reference=0;
    assert(draw(rejected,depth)==RDGPU_DRAW_OK); pixel(false,true);
    // Distinct front/back references must follow the declared winding.
    clear_depth(depth,1,0,3);
    state(mask).stencil[0].reference=3; state(mask).stencil[1].reference=7;
    state(mask).front_ccw=1;
    assert(draw(mask,depth)==RDGPU_DRAW_OK);
    for(auto &face:state(clipped).stencil) face.reference=3;
    assert(draw(clipped,depth)==RDGPU_DRAW_OK); pixel(true,false);
    state(mask).front_ccw=0;
    assert(draw(mask,depth)==RDGPU_DRAW_OK);
    for(auto &face:state(rejected).stencil) face.reference=7;
    assert(draw(rejected,depth)==RDGPU_DRAW_OK); pixel(false,true);
    state(rejected).stencil[0].compare=8;
    assert(draw(rejected,depth)==RDGPU_DRAW_BAD_PACKET);
    state(rejected).stencil[0].compare=2; state(rejected).color_write_mask=16;
    assert(draw(rejected,depth)==RDGPU_DRAW_BAD_PACKET);
    state(rejected).color_write_mask=15;
    assert(draw(rejected,nullptr)==RDGPU_DRAW_BAD_PACKET);
    std::vector<uint8_t> truncated(sizeof(rdmetal_draw_header));
    memcpy(truncated.data(),rejected.data(),truncated.size());
    ((rdmetal_draw_header *)truncated.data())->total_bytes=(uint32_t)truncated.size();
    assert(draw(truncated,depth)==RDGPU_DRAW_BAD_PACKET);
    RDMetalDepth *wrong_size=rd_metal_depth_create(core,8,8); assert(wrong_size);
    assert(draw(rejected,wrong_size)==RDGPU_DRAW_DENIED);
    rd_metal_depth_destroy(wrong_size);
    // V8 retains V7 depth state, but permits independent stage constants.
    std::vector<uint8_t> generic(far.size()+8);
    memcpy(generic.data(),far.data(),sizeof(rdmetal_draw_header_v7));
    memcpy(generic.data()+sizeof(rdmetal_draw_header_v8),
           far.data()+sizeof(rdmetal_draw_header_v7),far.size()-sizeof(rdmetal_draw_header_v7));
    auto &g=*(rdmetal_draw_header_v8 *)generic.data();
    auto &gh=*(rdmetal_draw_header *)generic.data();
    gh.version=RDMETAL_DRAW_VERSION_V8;gh.total_bytes=(uint32_t)generic.size();
    assert(draw(generic,depth)==RDGPU_DRAW_OK);pixel(false,true);
    // A positive Gallium viewport flips clip Y for offscreen FBOs.
    color=rd_metal_image_clear(image,0,0,0,1);assert(color && rd_metal_image_wait(image,color));
    g.vertex_flags=1;assert(draw(generic,depth)==RDGPU_DRAW_OK);
    assert(IOSurfaceLock(target,kIOSurfaceLockReadOnly,nullptr)==kIOReturnSuccess);
    const uint8_t *upper=(const uint8_t *)IOSurfaceGetBaseAddress(target)+2*IOSurfaceGetBytesPerRow(target)+2*4;
    assert(upper[1]>240 && upper[2]<10);
    IOSurfaceUnlock(target,kIOSurfaceLockReadOnly,nullptr);
    g.vertex_flags=0;
    g.fragment_uniform_bytes=RDMETAL_DRAW_MAX_STAGE_UNIFORM_BYTES+1;
    assert(draw(generic,depth)==RDGPU_DRAW_BAD_PACKET);
    g.fragment_uniform_bytes=0;g.vertex_flags=2;
    assert(draw(generic,depth)==RDGPU_DRAW_BAD_PACKET);
    g.vertex_flags=0;gh.fragment_shader_words=RDMETAL_DRAW_MAX_SHADER_WORDS;
    assert(draw(generic,depth)==RDGPU_DRAW_BAD_PACKET); // bounded before SPIR-V copy

    rd_metal_depth_destroy(depth); rd_metal_image_destroy(image); rd_metal_core_destroy(core);
    puts("METAL_DEPTH_STENCIL_DRAW_OK: depth occlusion, stencil clipping, masks, clears, validation");
}
