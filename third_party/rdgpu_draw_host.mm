#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "rdgpu_draw_host.h"
#include "rdmetal_draw_wire.h"
#include "spirv_msl.hpp"
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {
bool is_float_vec(const spirv_cross::CompilerMSL &compiler,
                  const spirv_cross::Resource &r, unsigned comps) {
    const auto &t = compiler.get_type(r.type_id);
    return t.basetype == spirv_cross::SPIRType::Float && t.columns == 1 &&
           t.vecsize == comps;
}

bool supported_resources(const spirv_cross::CompilerMSL &compiler,
                         spv::ExecutionModel stage, bool textured, bool v6) {
    const auto resources=compiler.get_shader_resources();
    if(!resources.uniform_buffers.empty()||!resources.storage_buffers.empty()||
       !resources.subpass_inputs.empty()||!resources.storage_images.empty()||
       !resources.atomic_counters.empty()||
       !resources.push_constant_buffers.empty()||!resources.separate_images.empty()||
       !resources.separate_samplers.empty()||!resources.gl_plain_uniforms.empty()||
       !resources.acceleration_structures.empty())return false;
    if(stage==spv::ExecutionModelVertex){
        if(v6){
            /* Real Qt shape: position(float2)@0 + color(vec4)@1 + opacity(float)@2
             * with color(vec4)+opacity(float) varyings. */
            if(resources.stage_inputs.size()!=3)return false;
            int have_pos=0,have_color=0,have_opacity=0;
            for(const auto &input:resources.stage_inputs){
                uint32_t loc=compiler.get_decoration(input.id,spv::DecorationLocation);
                if(loc==0&&is_float_vec(compiler,input,2))have_pos=1;
                else if(loc==1&&is_float_vec(compiler,input,4))have_color=1;
                else if(loc==2&&is_float_vec(compiler,input,1))have_opacity=1;
                else return false;
            }
            if(!have_pos||!have_color||!have_opacity)return false;
            if(resources.stage_outputs.size()!=2)return false;
            int out_color=0,out_opacity=0;
            for(const auto &out:resources.stage_outputs){
                if(is_float_vec(compiler,out,4))out_color=1;
                else if(is_float_vec(compiler,out,1))out_opacity=1;
                else return false;
            }
            return out_color&&out_opacity;
        }
        /* Canonical two-attribute layout: float3 position at location 0 and
         * float2 texcoord at location 1, plus the forwarded varying at
         * location 0. */
        if(resources.stage_inputs.size()!=2)return false;
        int have_pos=0,have_uv=0;
        for(const auto &input:resources.stage_inputs){
            const auto &type=compiler.get_type(input.type_id);
            if(type.basetype!=spirv_cross::SPIRType::Float||type.columns!=1)return false;
            uint32_t loc=compiler.get_decoration(input.id,spv::DecorationLocation);
            if(loc==0&&type.vecsize==3)have_pos=1;
            else if(loc==1&&type.vecsize==2)have_uv=1;
            else return false;
        }
        if(!have_pos||!have_uv)return false;
        /* The synthetic vertex stage emits a forwarded float2 UV varying; a
         * fixed-color fragment stage simply does not consume it. */
        if(resources.stage_outputs.size()>1)return false;
        if(!resources.stage_outputs.empty()){
            const auto &out=resources.stage_outputs[0];
            const auto &otype=compiler.get_type(out.type_id);
            if(compiler.get_decoration(out.id,spv::DecorationLocation)!=0||
               otype.basetype!=spirv_cross::SPIRType::Float||
               otype.vecsize!=2||otype.columns!=1)return false;
        }
        return true;
    }
    /* fragment */
    if(v6){
        /* Real Qt shape: color(vec4)+opacity(float) varyings, at most one
         * sampled image (solid-color rectangles have none) and one color
         * output. */
        if(resources.stage_inputs.size()!=2||
           resources.stage_outputs.size()!=1||
           resources.sampled_images.size()>(textured?1u:0u))return false;
        if(textured&&resources.sampled_images.size()!=1)return false;
        if(!textured&&!resources.sampled_images.empty())return false;
        int in_color=0,in_opacity=0;
        for(const auto &input:resources.stage_inputs){
            if(is_float_vec(compiler,input,4))in_color=1;
            else if(is_float_vec(compiler,input,1))in_opacity=1;
            else return false;
        }
        if(!in_color||!in_opacity)return false;
        const auto &output=resources.stage_outputs[0];
        return is_float_vec(compiler,output,4)&&
               compiler.get_decoration(output.id,spv::DecorationLocation)==0;
    }
    if(textured){
        if(resources.stage_inputs.size()!=1||
           resources.sampled_images.size()!=1||
           resources.stage_outputs.size()!=1)return false;
        const auto &input=resources.stage_inputs[0];
        const auto &type=compiler.get_type(input.type_id);
        if(compiler.get_decoration(input.id,spv::DecorationLocation)!=0||
           type.basetype!=spirv_cross::SPIRType::Float||
           type.vecsize!=2||type.columns!=1)return false;
    } else {
        if(!resources.stage_inputs.empty()||
           !resources.sampled_images.empty()||
           resources.stage_outputs.size()!=1)return false;
    }
    return compiler.get_decoration(resources.stage_outputs[0].id,
                                   spv::DecorationLocation)==0;
}
bool checked_header(const rdmetal_draw_header &h,size_t bytes,size_t prefix){
    uint32_t attr_size = h.attribute_format==RDMETAL_DRAW_ATTR_FLOAT2 ? 8 :
                         h.attribute_format==RDMETAL_DRAW_ATTR_FLOAT3 ? 12 : 0;
    if(h.magic!=RDMETAL_DRAW_MAGIC||
       (h.version!=RDMETAL_DRAW_VERSION&&
        h.version!=RDMETAL_DRAW_VERSION_TEXTURE&&
        h.version!=RDMETAL_DRAW_VERSION_V3&&
        h.version!=RDMETAL_DRAW_VERSION_V4&&
        h.version!=RDMETAL_DRAW_VERSION_V5&&
        h.version!=RDMETAL_DRAW_VERSION_V6)||
       h.flags||!h.target_handle||h.total_bytes!=bytes||
       !h.target_width||!h.target_height||
       h.target_width>4096||h.target_height>4096||
       uint64_t(h.target_width)*h.target_height*4>64u*1024u*1024u||
       h.vertex_shader_words<5||h.fragment_shader_words<5||
       h.vertex_shader_words>RDMETAL_DRAW_MAX_SHADER_WORDS||
       h.fragment_shader_words>RDMETAL_DRAW_MAX_SHADER_WORDS||
       !h.vertex_bytes||h.vertex_bytes>RDMETAL_DRAW_MAX_VERTEX_BYTES||
       !attr_size||
       h.vertex_stride<attr_size||h.vertex_stride>256||
       h.attribute_offset>h.vertex_stride-attr_size||
       !h.vertex_count||h.vertex_count>65535||h.vertex_count%3||
       h.vertex_start>65535)return false;
    if(h.version==RDMETAL_DRAW_VERSION&&prefix!=sizeof(rdmetal_draw_header))return false;
    if(h.version==RDMETAL_DRAW_VERSION_TEXTURE&&prefix!=sizeof(rdmetal_draw_header_v2))return false;
    if(h.version==RDMETAL_DRAW_VERSION_V3&&prefix!=sizeof(rdmetal_draw_header_v3))return false;
    if(h.version==RDMETAL_DRAW_VERSION_V4&&prefix!=sizeof(rdmetal_draw_header_v4))return false;
    if(h.version==RDMETAL_DRAW_VERSION_V5&&prefix!=sizeof(rdmetal_draw_header_v5))return false;
    if(h.version==RDMETAL_DRAW_VERSION_V6&&prefix!=sizeof(rdmetal_draw_header_v6))return false;
    /* The v3 tail (attr1/index/uniform) and the exact vertex-bounds check are
     * validated by rdgpu_draw_execute once the index data is available. */
    return true;
}
} // namespace

int rdgpu_draw_execute_with_depth(const void *packet,size_t bytes,
                       RDGPUDrawResolve resolve,void *context,
                       RDGPUDrawTextureResolve tex_resolve,
                       RDGPUDrawTextureComplete tex_complete,
                       void *tex_context,void *depth_texture){
    if(!packet||!resolve||bytes<sizeof(rdmetal_draw_header)||
       bytes>sizeof(rdmetal_draw_header_v6)+2*RDMETAL_DRAW_MAX_SHADER_WORDS*4+
             RDMETAL_DRAW_MAX_VERTEX_BYTES+RDMETAL_DRAW_MAX_INDEX_BYTES+
             RDMETAL_DRAW_MAX_UNIFORM_BYTES)return RDGPU_DRAW_BAD_PACKET;
    rdmetal_draw_header h;
    memcpy(&h,packet,sizeof h);
    const bool v6 = h.version==RDMETAL_DRAW_VERSION_V6;
    const bool v5 = h.version==RDMETAL_DRAW_VERSION_V5 || v6;
    const bool v4 = h.version==RDMETAL_DRAW_VERSION_V4 || v5;
    const bool v3 = h.version==RDMETAL_DRAW_VERSION_V3 || v4;
    const bool v2tex = h.version==RDMETAL_DRAW_VERSION_TEXTURE;
    const size_t prefix = v6 ? sizeof(rdmetal_draw_header_v6) :
                          v5 ? sizeof(rdmetal_draw_header_v5) :
                          v4 ? sizeof(rdmetal_draw_header_v4) :
                          v3 ? sizeof(rdmetal_draw_header_v3) :
                          v2tex ? sizeof(rdmetal_draw_header_v2) :
                                  sizeof(rdmetal_draw_header);
    if(!checked_header(h,bytes,prefix))return RDGPU_DRAW_BAD_PACKET;
    uint32_t depth_flags=0,depth_compare=7;
    bool has_depth=false;
    if(v5){
        rdmetal_draw_header_v5 d;
        memcpy(&d,packet,sizeof d);
        if(d.reserved||(d.depth_flags&~3u)||d.depth_compare>7)
            return RDGPU_DRAW_BAD_PACKET;
        if(d.depth_handle){
            if(!depth_texture)return RDGPU_DRAW_BAD_PACKET;
            depth_flags=d.depth_flags;depth_compare=d.depth_compare;
            has_depth=true;
        } else if(!v6){
            /* V5 always carries a depth lease; only V6 may omit one. */
            return RDGPU_DRAW_BAD_PACKET;
        }
    }
    uint32_t scissor_x=0,scissor_y=0;
    uint32_t scissor_width=h.target_width,scissor_height=h.target_height;
    if(v4){
        rdmetal_draw_header_v4 v4h;
        memcpy(&v4h,packet,sizeof v4h);
        scissor_x=v4h.scissor_x;scissor_y=v4h.scissor_y;
        scissor_width=v4h.scissor_width;scissor_height=v4h.scissor_height;
        if(scissor_x>=h.target_width||scissor_y>=h.target_height||
           !scissor_width||!scissor_height||
           scissor_width>h.target_width-scissor_x||
           scissor_height>h.target_height-scissor_y)
            return RDGPU_DRAW_BAD_PACKET;
    }
    uint64_t texture_handle=0,texture_generation=0;
    uint32_t attr1_format=RDMETAL_DRAW_ATTR_NONE,attr1_offset=0;
    uint32_t attr2_format=RDMETAL_DRAW_ATTR_NONE,attr2_offset=0;
    uint32_t index_type=RDMETAL_DRAW_INDEX_NONE,index_count=0,uniform_bytes=0;
    bool textured = v2tex; /* v2 is always textured; v3 is textured iff a
                            * non-zero texture handle is present. */
    if(v3){
        rdmetal_draw_header_v3 v3h;
        memcpy(&v3h,packet,sizeof v3h);
        if(v3h.reserved)return RDGPU_DRAW_BAD_PACKET;
        texture_handle=v3h.base.texture_handle;
        texture_generation=v3h.base.texture_generation;
        if(v3h.base.sampler_flags||v3h.base.reserved)return RDGPU_DRAW_BAD_PACKET;
        attr1_format=v3h.attr1_format;
        attr1_offset=v3h.attr1_offset;
        index_type=v3h.index_type;
        index_count=v3h.index_count;
        uniform_bytes=v3h.uniform_bytes;
        textured = texture_handle != 0;
        if(v6){
            /* Real Qt layout: attr1 is color (u8x4 unorm) before opacity. */
            if(attr1_format!=RDMETAL_DRAW_ATTR_U8X4_UNORM||
               attr1_offset+4>h.vertex_stride)return RDGPU_DRAW_BAD_PACKET;
        } else {
            /* attr1 must be float2 (texcoord) at a sane offset; canonical
             * layout is float3 pos + float2 uv. */
            if(attr1_format!=RDMETAL_DRAW_ATTR_FLOAT2||
               attr1_offset+8>h.vertex_stride)return RDGPU_DRAW_BAD_PACKET;
        }
        if(index_type!=RDMETAL_DRAW_INDEX_NONE){
            uint32_t isize = index_type==RDMETAL_DRAW_INDEX_U16 ? 2 :
                             index_type==RDMETAL_DRAW_INDEX_U32 ? 4 : 0;
            if(!isize||!index_count||index_count>65535||index_count%3||
               uint64_t(index_count)*isize>RDMETAL_DRAW_MAX_INDEX_BYTES)
                return RDGPU_DRAW_BAD_PACKET;
        } else if(index_count)return RDGPU_DRAW_BAD_PACKET;
        if(uniform_bytes&&uniform_bytes!=RDMETAL_DRAW_MAX_UNIFORM_BYTES)
            return RDGPU_DRAW_BAD_PACKET;
        if(textured&&(!texture_handle||!texture_generation))return RDGPU_DRAW_BAD_PACKET;
        if(!textured&&texture_generation)return RDGPU_DRAW_BAD_PACKET;
        if(v6){
            rdmetal_draw_header_v6 v6h;
            memcpy(&v6h,packet,sizeof v6h);
            attr2_format=v6h.attr2_format;
            attr2_offset=v6h.attr2_offset;
            /* attr2 is the R32_FLOAT opacity following color in the vertex. */
            if(attr2_format!=RDMETAL_DRAW_ATTR_FLOAT||
               attr2_offset+4>h.vertex_stride)return RDGPU_DRAW_BAD_PACKET;
        }
    } else if(v2tex){
        rdmetal_draw_header_v2 v2;
        memcpy(&v2,packet,sizeof v2);
        if(!v2.texture_handle||!v2.texture_generation||
           v2.sampler_flags||v2.reserved)return RDGPU_DRAW_BAD_PACKET;
        if(!tex_resolve)return RDGPU_DRAW_DENIED;
        texture_handle=v2.texture_handle;
        texture_generation=v2.texture_generation;
    }
    const uint8_t *payload=(const uint8_t *)packet+prefix;
    std::vector<uint32_t> vs(h.vertex_shader_words),fs(h.fragment_shader_words);
    memcpy(vs.data(),payload,vs.size()*4);payload+=vs.size()*4;
    memcpy(fs.data(),payload,fs.size()*4);payload+=fs.size()*4;
    const uint8_t *vertex_data=payload;
    const uint8_t *index_data=payload+h.vertex_bytes;
    const uint8_t *uniform_data=index_data+(index_type!=RDMETAL_DRAW_INDEX_NONE?
        uint64_t(index_count)*(index_type==RDMETAL_DRAW_INDEX_U16?2:4):0);
    (void)uniform_data; /* reserved for the uniform/constant-buffer slice */
    /* Verify the tail reaches exactly the end of the packet. */
    if(v3){
        uint64_t total=uint64_t(prefix)+uint64_t(vs.size())*4+uint64_t(fs.size())*4+
            uint64_t(h.vertex_bytes)+
            uint64_t(index_type!=RDMETAL_DRAW_INDEX_NONE?
                uint64_t(index_count)*(index_type==RDMETAL_DRAW_INDEX_U16?2:4):0)+
            uint64_t(uniform_bytes);
        if(total!=bytes)return RDGPU_DRAW_BAD_PACKET;
    }
    /* Vertex-bounds check: for indexed draws the vertex buffer must cover the
     * largest referenced index; for non-indexed it must cover vertex_start +
     * vertex_count - 1. */
    uint32_t attr_size = h.attribute_format==RDMETAL_DRAW_ATTR_FLOAT2 ? 8 : 12;
    if(index_type!=RDMETAL_DRAW_INDEX_NONE){
        uint32_t max_index=0;
        if(index_type==RDMETAL_DRAW_INDEX_U16){
            const uint16_t *ix=(const uint16_t *)index_data;
            for(unsigned i=0;i<index_count;i++)if(ix[i]>max_index)max_index=ix[i];
        } else {
            const uint32_t *ix=(const uint32_t *)index_data;
            for(unsigned i=0;i<index_count;i++)if(ix[i]>max_index)max_index=ix[i];
        }
        uint64_t needed=(uint64_t)(max_index+1)*h.vertex_stride;
        if(needed>h.vertex_bytes)return RDGPU_DRAW_BAD_PACKET;
    } else {
        uint64_t needed=(uint64_t)(h.vertex_start+h.vertex_count-1)*
            h.vertex_stride+h.attribute_offset+attr_size;
        if(needed>h.vertex_bytes)return RDGPU_DRAW_BAD_PACKET;
    }
    if(vs[0]!=spv::MagicNumber||fs[0]!=spv::MagicNumber)return RDGPU_DRAW_BAD_PACKET;
    if(getenv("RDMETAL_TRACE_SPIRV")){
        fprintf(stderr,"RDMETAL_SPIRV vs_words=%u fs_words=%u textured=%d v3=%d idx=%u\n",
            (unsigned)vs.size(),(unsigned)fs.size(),textured?1:0,v3?1:0,index_count);
    }
    std::string vertexMSL,fragmentMSL,vertexEntry,fragmentEntry;
    try {
        spirv_cross::CompilerMSL vertex(vs),fragment(fs);
        if(vertex.get_execution_model()!=spv::ExecutionModelVertex||
           fragment.get_execution_model()!=spv::ExecutionModelFragment||
           !supported_resources(vertex,spv::ExecutionModelVertex,textured,v6)||
           !supported_resources(fragment,spv::ExecutionModelFragment,textured,v6))
            return RDGPU_DRAW_UNSUPPORTED;
        auto vertexPoints=vertex.get_entry_points_and_stages();
        auto fragmentPoints=fragment.get_entry_points_and_stages();
        if(vertexPoints.size()!=1||fragmentPoints.size()!=1)
            return RDGPU_DRAW_UNSUPPORTED;
        vertexMSL=vertex.compile();fragmentMSL=fragment.compile();
        if(getenv("RDMETAL_TRACE_SPIRV")){
            fprintf(stderr,"RDMETAL_MSL_VS\n%s\nRDMETAL_MSL_FS\n%s\n",vertexMSL.c_str(),fragmentMSL.c_str());
        }
        vertexEntry=vertex.get_cleansed_entry_point_name(
            vertexPoints[0].name,spv::ExecutionModelVertex);
        fragmentEntry=fragment.get_cleansed_entry_point_name(
            fragmentPoints[0].name,spv::ExecutionModelFragment);
    } catch(const std::exception &e) {
        fprintf(stderr,"RDMETAL_DRAW_SPIRV_CROSS_FAIL %s\n",e.what());
        return RDGPU_DRAW_SHADER_FAILED;
    }
    IOSurfaceRef surface=resolve(h.target_handle,context);
    if(!surface){
        fprintf(stderr,"RDMETAL_DRAW_RESOLVE_DENIED handle=%llu\n",
                (unsigned long long)h.target_handle);
        return RDGPU_DRAW_DENIED;
    }
    void *tex=textured?tex_resolve(texture_handle,texture_generation,tex_context):NULL;
    IOSurfaceRef texSurface=NULL;
    if(textured&&!tex){
        /* The texture handle may be a direct render-target lease (a previous
         * frame's IOSurface sampled by a blur pass) instead of a promoted
         * sample texture. Resolve it exactly like the target and wrap the
         * IOSurface as a shader-read texture below. */
        texSurface=resolve(texture_handle,context);
        if(!texSurface){
            CFRelease(surface);
            return RDGPU_DRAW_DENIED;
        }
    }
    int result=RDGPU_DRAW_GPU_FAILED;
    @autoreleasepool {
        if(IOSurfaceGetWidth(surface)==h.target_width&&
           IOSurfaceGetHeight(surface)==h.target_height){
            id<MTLDevice> device=MTLCreateSystemDefaultDevice();
            NSError *error=nil;
            NSString *vSource=[[NSString alloc] initWithBytes:vertexMSL.data()
                length:vertexMSL.size() encoding:NSUTF8StringEncoding];
            NSString *fSource=[[NSString alloc] initWithBytes:fragmentMSL.data()
                length:fragmentMSL.size() encoding:NSUTF8StringEncoding];
            id<MTLLibrary> vLib=vSource?[device newLibraryWithSource:vSource
                options:nil error:&error]:nil;
            id<MTLLibrary> fLib=fSource?[device newLibraryWithSource:fSource
                options:nil error:&error]:nil;
            id<MTLFunction> vFunction=vLib?[vLib newFunctionWithName:
                [NSString stringWithUTF8String:vertexEntry.c_str()]]:nil;
            id<MTLFunction> fFunction=fLib?[fLib newFunctionWithName:
                [NSString stringWithUTF8String:fragmentEntry.c_str()]]:nil;
            if(!device||!vFunction||!fFunction){
                result=RDGPU_DRAW_SHADER_FAILED;
                fprintf(stderr,"RDMETAL_DRAW_MSL_FAIL device=%d vFn=%d fFn=%d verr=%s ferr=%s\n",
                        device!=nil,vFunction!=nil,fFunction!=nil,
                        error?[[error localizedDescription] UTF8String]:"",
                        error?[[error localizedDescription] UTF8String]:"");
            }
            else {
                MTLTextureDescriptor *desc=[MTLTextureDescriptor
                    texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                    width:h.target_width height:h.target_height mipmapped:NO];
                desc.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
                id<MTLTexture> target=[device newTextureWithDescriptor:desc
                    iosurface:surface plane:0];
                id<MTLTexture> texAsMetal=tex?(__bridge id<MTLTexture>)tex:nil;
                if(textured&&!texAsMetal&&texSurface){
                    MTLTextureDescriptor *texDesc=[MTLTextureDescriptor
                        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                        width:(NSUInteger)IOSurfaceGetWidth(texSurface)
                        height:(NSUInteger)IOSurfaceGetHeight(texSurface)
                        mipmapped:NO];
                    texDesc.usage=MTLTextureUsageShaderRead;
                    texAsMetal=[device newTextureWithDescriptor:texDesc
                        iosurface:texSurface plane:0];
                }
                MTLRenderPipelineDescriptor *pipeline=[MTLRenderPipelineDescriptor new];
                pipeline.vertexFunction=vFunction;pipeline.fragmentFunction=fFunction;
                pipeline.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;
                /* Straight-alpha blending: Qt's glyph/UI quads output color in
                 * RGB and coverage in A. Without this the whole textured quad
                 * renders opaque (white rectangles), so text and translucent
                 * controls are unreadable. */
                pipeline.colorAttachments[0].blendingEnabled=YES;
                pipeline.colorAttachments[0].rgbBlendOperation=MTLBlendOperationAdd;
                pipeline.colorAttachments[0].alphaBlendOperation=MTLBlendOperationAdd;
                pipeline.colorAttachments[0].sourceRGBBlendFactor=MTLBlendFactorSourceAlpha;
                pipeline.colorAttachments[0].sourceAlphaBlendFactor=MTLBlendFactorOne;
                pipeline.colorAttachments[0].destinationRGBBlendFactor=MTLBlendFactorOneMinusSourceAlpha;
                pipeline.colorAttachments[0].destinationAlphaBlendFactor=MTLBlendFactorOneMinusSourceAlpha;
                if(has_depth){
                    id<MTLTexture> depth=(__bridge id<MTLTexture>)depth_texture;
                    if(depth.width!=h.target_width||depth.height!=h.target_height||
                       depth.pixelFormat!=MTLPixelFormatDepth32Float_Stencil8){
                        result=RDGPU_DRAW_DENIED;
                        goto draw_cleanup;
                    }
                    pipeline.depthAttachmentPixelFormat=MTLPixelFormatDepth32Float_Stencil8;
                    pipeline.stencilAttachmentPixelFormat=MTLPixelFormatDepth32Float_Stencil8;
                }
                MTLVertexDescriptor *layout=[MTLVertexDescriptor vertexDescriptor];
                layout.attributes[0].format=
                    h.attribute_format==RDMETAL_DRAW_ATTR_FLOAT3?
                        MTLVertexFormatFloat3:MTLVertexFormatFloat2;
                layout.attributes[0].offset=h.attribute_offset;
                layout.attributes[0].bufferIndex=0;
                if(v3){
                    layout.attributes[1].format=
                        attr1_format==RDMETAL_DRAW_ATTR_U8X4_UNORM?
                            MTLVertexFormatUChar4Normalized:MTLVertexFormatFloat2;
                    layout.attributes[1].offset=attr1_offset;
                    layout.attributes[1].bufferIndex=0;
                }
                if(v6){
                    layout.attributes[2].format=MTLVertexFormatFloat;
                    layout.attributes[2].offset=attr2_offset;
                    layout.attributes[2].bufferIndex=0;
                }
                layout.layouts[0].stride=h.vertex_stride;
                layout.layouts[0].stepFunction=MTLVertexStepFunctionPerVertex;
                layout.layouts[0].stepRate=1;
                pipeline.vertexDescriptor=layout;
                id<MTLRenderPipelineState> state=[device
                    newRenderPipelineStateWithDescriptor:pipeline error:&error];
                id<MTLCommandQueue> queue=[device newCommandQueue];
                id<MTLBuffer> vertex=[device newBufferWithBytes:vertex_data
                    length:h.vertex_bytes options:MTLResourceStorageModeShared];
                MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
                pass.colorAttachments[0].texture=target;
                pass.colorAttachments[0].loadAction=MTLLoadActionLoad;
                pass.colorAttachments[0].storeAction=MTLStoreActionStore;
                pass.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,1);
                if(has_depth){
                    id<MTLTexture> depth=(__bridge id<MTLTexture>)depth_texture;
                    pass.depthAttachment.texture=depth;
                    pass.depthAttachment.loadAction=MTLLoadActionLoad;
                    pass.depthAttachment.storeAction=MTLStoreActionStore;
                    pass.stencilAttachment.texture=depth;
                    pass.stencilAttachment.loadAction=MTLLoadActionLoad;
                    pass.stencilAttachment.storeAction=MTLStoreActionStore;
                }
                id<MTLCommandBuffer> command=[queue commandBuffer];
                id<MTLRenderCommandEncoder> encoder=command&&target&&state&&vertex?
                    [command renderCommandEncoderWithDescriptor:pass]:nil;
                if(encoder){
                    [encoder setRenderPipelineState:state];
                    if(has_depth){
                        static const MTLCompareFunction compare[]={
                            MTLCompareFunctionNever,MTLCompareFunctionLess,
                            MTLCompareFunctionEqual,MTLCompareFunctionLessEqual,
                            MTLCompareFunctionGreater,MTLCompareFunctionNotEqual,
                            MTLCompareFunctionGreaterEqual,MTLCompareFunctionAlways};
                        MTLDepthStencilDescriptor *ds=[MTLDepthStencilDescriptor new];
                        ds.depthCompareFunction=(depth_flags&1u)?compare[depth_compare]:MTLCompareFunctionAlways;
                        ds.depthWriteEnabled=(depth_flags&2u)!=0;
                        [encoder setDepthStencilState:[device newDepthStencilStateWithDescriptor:ds]];
                    }
                    [encoder setVertexBuffer:vertex offset:0 atIndex:0];
                    MTLScissorRect scissor={scissor_x,scissor_y,
                                            scissor_width,scissor_height};
                    [encoder setScissorRect:scissor];
                    if(textured){
                        MTLSamplerDescriptor *samplerDesc=[MTLSamplerDescriptor new];
                        samplerDesc.minFilter=MTLSamplerMinMagFilterNearest;
                        samplerDesc.magFilter=MTLSamplerMinMagFilterNearest;
                        samplerDesc.mipFilter=MTLSamplerMipFilterNotMipmapped;
                        samplerDesc.sAddressMode=MTLSamplerAddressModeClampToEdge;
                        samplerDesc.tAddressMode=MTLSamplerAddressModeClampToEdge;
                        id<MTLSamplerState> sampler=[device
                            newSamplerStateWithDescriptor:samplerDesc];
                        [encoder setFragmentTexture:texAsMetal
                            atIndex:0];
                        [encoder setFragmentSamplerState:sampler atIndex:0];
                    }
                    if(index_type!=RDMETAL_DRAW_INDEX_NONE){
                        uint32_t isize = index_type==RDMETAL_DRAW_INDEX_U16 ? 2 : 4;
                        id<MTLBuffer> index=[device newBufferWithBytes:index_data
                            length:index_count*isize options:MTLResourceStorageModeShared];
                        [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                            indexCount:index_count
                            indexType:index_type==RDMETAL_DRAW_INDEX_U16?
                                MTLIndexTypeUInt16:MTLIndexTypeUInt32
                            indexBuffer:index indexBufferOffset:0];
                    } else {
                        [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                            vertexStart:h.vertex_start vertexCount:h.vertex_count];
                    }
                    [encoder endEncoding];[command commit];[command waitUntilCompleted];
                    result=command.status==MTLCommandBufferStatusCompleted?
                        RDGPU_DRAW_OK:RDGPU_DRAW_GPU_FAILED;
                    if(result==RDGPU_DRAW_OK&&getenv("RDMETAL_TRACE_SPIRV"))
                        fprintf(stderr,"RDMETAL_DRAW_SAMPLED textured=%d ok\n",textured?1:0);
                }
            }
            draw_cleanup: ;
        } else {
            fprintf(stderr,"RDMETAL_DRAW_DIMS_DENIED sw=%zu sh=%zu tw=%u th=%u\n",
                    IOSurfaceGetWidth(surface),IOSurfaceGetHeight(surface),
                    h.target_width,h.target_height);
            result=RDGPU_DRAW_DENIED;
        }
    }
    if(textured&&tex_complete)tex_complete(texture_handle,texture_generation,tex_context);
    if(texSurface)CFRelease(texSurface);
    CFRelease(surface);
    return result;
}

int rdgpu_draw_execute(const void *packet,size_t bytes,
                       RDGPUDrawResolve resolve,void *context,
                       RDGPUDrawTextureResolve tex_resolve,
                       RDGPUDrawTextureComplete tex_complete,
                       void *tex_context){
    return rdgpu_draw_execute_with_depth(packet,bytes,resolve,context,
                                         tex_resolve,tex_complete,tex_context,NULL);
}
