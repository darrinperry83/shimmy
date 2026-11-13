// glbridge.m — fixed-function OpenGL on Metal. See glbridge.h for the contract.
//
// Design notes that matter for speed (a previous attempt opened a render pass per draw and was
// ~140x slower):
//   * ONE MTLRenderCommandEncoder per frame. Opened lazily; glb_clear ends it and reopens a
//     cleared one; glb_swap ends + commits and does NOT wait.
//   * A triple-buffered ring of large MTLStorageModeShared buffers. Vertices and per-draw uniforms
//     are bump-allocated out of the current slot. A dispatch_semaphore bounds frames in flight so
//     the GPU can never be reading a slot the CPU is overwriting. Zero device allocations on the
//     hot path.
//   * Pipeline / depth-stencil / sampler states are cached in dictionaries keyed by the state that
//     selects them, and encoder state is only re-set when it actually changed.
//   * Shader permutations are compiled from one inline MSL source using function constants.
//
// Coordinate conventions: GL clip space is y-up, z in [-1,1]; Metal is y-up but z in [0,1] and the
// render target's origin is top-left. We therefore fold a correction matrix C into the MVP
// (y := -y, z := (z+w)/2) so the image lands right-way-up in a top-left-origin texture, and invert
// the front-facing winding to compensate for the handedness flip C introduces.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <simd/simd.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include "glbridge.h"

// ------------------------------------------------------------------------------- tunables -----
#define GLB_RING_SLOTS      GLB_FRAMES_IN_FLIGHT
#define GLB_RING_BYTES      (6u * 1024u * 1024u)
#define GLB_ALIGN           256u
#define GLB_MAX_STACK       64
#define GLB_MAX_LIGHTS      8
#define GLB_MAX_LIST_DEPTH  16

// ------------------------------------------------------------------------------ structures ----
typedef struct {
    float pos[3];
    float nrm[3];
    float col[4];
    float uv[2];
} GlbVertex;                       // 48 bytes; mirrored by `Vtx` in the MSL below.

typedef struct {
    simd_float4x4 mvp;
    simd_float4x4 mv;
    simd_float4x4 nrm;
    simd_float4   lpos[GLB_MAX_LIGHTS];
    simd_float4   lamb[GLB_MAX_LIGHTS];
    simd_float4   ldif[GLB_MAX_LIGHTS];
    simd_float4   lspc[GLB_MAX_LIGHTS];
    simd_float4   mAmb;
    simd_float4   mDif;
    simd_float4   mSpc;
    simd_float4   mEmi;
    simd_float4   sceneAmb;
    simd_float4   misc;            // x=shininess y=pointSize
} GlbUniforms;

// ---- display-list command record ----
enum {
    OP_MATRIX_MODE = 1, OP_LOAD_IDENTITY, OP_LOAD_MATRIX, OP_MULT_MATRIX,
    OP_PUSH, OP_POP, OP_TRANSLATE, OP_ROTATE, OP_SCALE, OP_FRUSTUM, OP_ORTHO,
    OP_BEGIN, OP_VERTEX, OP_NORMAL, OP_COLOR, OP_TEXCOORD, OP_END,
    OP_ENABLE, OP_DISABLE, OP_DEPTH_FUNC, OP_DEPTH_MASK, OP_CULL_FACE, OP_FRONT_FACE,
    OP_SHADE_MODEL, OP_BLEND_FUNC, OP_POINT_SIZE,
    OP_LIGHT, OP_MATERIAL, OP_LIGHT_MODEL, OP_COLOR_MATERIAL,
    OP_BIND_TEXTURE, OP_VIEWPORT, OP_CLEAR, OP_CLEAR_COLOR, OP_CLEAR_DEPTH, OP_CALL_LIST,
};

typedef struct {
    uint32_t op;
    uint32_t a, b;
    float    f[16];
} GlbCmd;

typedef struct {
    GlbCmd*  cmds;
    uint32_t count, cap;
    int      used;
} GlbList;

typedef struct {
    id<MTLBuffer> buf;
    NSUInteger    off;
    void*         ptr;
} GlbAlloc;

// --------------------------------------------------------------------------- global state -----
static struct {
    int inited;

    // Metal objects
    id<MTLDevice>               dev;
    id<MTLCommandQueue>         queue;
    id<MTLLibrary>              lib;
    id<MTLTexture>              color, depth, white;
    NSMutableDictionary*        pipeCache;      // NSNumber(key) -> id<MTLRenderPipelineState>
    NSMutableDictionary*        depthCache;     // NSNumber(key) -> id<MTLDepthStencilState>
    NSMutableDictionary*        samplerCache;   // NSNumber(key) -> id<MTLSamplerState>
    NSMutableDictionary*        textures;       // NSNumber(id)  -> id<MTLTexture>

    // ring
    id<MTLBuffer>               ring[GLB_RING_SLOTS];
    NSMutableArray*             overflow[GLB_RING_SLOTS];
    NSUInteger                  ringOff;
    int                         slot;
    dispatch_semaphore_t        sem;

    // frame
    id<MTLCommandBuffer>        cmdBuf, lastCmdBuf;
    id<MTLRenderCommandEncoder> enc;
    int                         frameOpen;
    uint64_t                    frameCounter;
    uint64_t                    drawsThisFrame, vertsThisFrame, drawsLast, vertsLast;

    // framebuffer
    int fbW, fbH;

    // pending clear (consumed when the next encoder is opened)
    uint32_t pendingClearMask;
    float    pendingClear[4], pendingClearDepth;
    float    clearColor[4], clearDepth;

    // encoder state cache (reset when an encoder is opened)
    uint64_t curPipeKey, curDepthKey;
    int      curCull, curWinding;
    id<MTLBuffer> boundVB, boundVU, boundFU;
    id<MTLTexture> boundTex;
    id<MTLSamplerState> boundSmp;
    int      viewportDirty;

    // ---- GL state ----
    uint32_t matrixMode;
    float    mv[GLB_MAX_STACK][16];   int mvTop;
    float    pr[GLB_MAX_STACK][16];   int prTop;
    float    tx[GLB_MAX_STACK][16];   int txTop;

    int      enDepth, enLighting, enCull, enNormalize, enTexture, enBlend, enColorMat;
    uint32_t lightEnabled;            // bitmask
    uint32_t depthFunc; int depthWrite;
    uint32_t cullFace, frontFace, shadeModel, blendSrc, blendDst;
    float    pointSize;
    uint32_t colorMatMode;

    float    lpos[GLB_MAX_LIGHTS][4];  // already in EYE space
    float    lamb[GLB_MAX_LIGHTS][4];
    float    ldif[GLB_MAX_LIGHTS][4];
    float    lspc[GLB_MAX_LIGHTS][4];
    float    mAmb[4], mDif[4], mSpc[4], mEmi[4], shininess;
    float    sceneAmb[4];
    int      twoSided, localViewer;

    int      vpX, vpY, vpW, vpH;
    uint32_t boundTexId;
    uint32_t texMin, texMag, texWrapS, texWrapT;

    // immediate mode
    int         inBegin;
    uint32_t    prim;
    GlbVertex*  imVerts;
    uint32_t    imCount, imCap;
    float       curNormal[3], curColor[4], curUV[2];

    // uniform caching within a frame
    int         uniformsDirty;
    GlbAlloc    uniformsAlloc;

    // display lists
    GlbList*    lists;
    uint32_t    listCount, listCap;      // lists[] indexed by id-1
    uint32_t    recording;               // list id being recorded, 0 = none
    int         recExec;                 // GLB_COMPILE_AND_EXECUTE
    int         replayDepth;
} G;

// ------------------------------------------------------------------------------- matrices -----
static void mat_identity(float* m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

// c = a * b, column-major (c_col_j = a * b_col_j)
static void mat_mul(float* c, const float* a, const float* b) {
    float t[16];
    for (int j = 0; j < 4; j++) {
        for (int i = 0; i < 4; i++) {
            t[j * 4 + i] = a[0 * 4 + i] * b[j * 4 + 0] + a[1 * 4 + i] * b[j * 4 + 1] +
                           a[2 * 4 + i] * b[j * 4 + 2] + a[3 * 4 + i] * b[j * 4 + 3];
        }
    }
    memcpy(c, t, sizeof(t));
}

static float* cur_stack_top(void) {
    switch (G.matrixMode) {
        case GLB_PROJECTION: return G.pr[G.prTop];
        case GLB_TEXTURE:    return G.tx[G.txTop];
        default:             return G.mv[G.mvTop];
    }
}

static void mat_post(const float* m) {
    float* c = cur_stack_top();
    mat_mul(c, c, m);
    G.uniformsDirty = 1;
}

// 3x3 inverse-transpose of the modelview's upper-left, expanded to a 4x4.
static void normal_matrix(const float* mv, float* out) {
    float a = mv[0], b = mv[4], c = mv[8];
    float d = mv[1], e = mv[5], f = mv[9];
    float g = mv[2], h = mv[6], i = mv[10];
    float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    mat_identity(out);
    if (fabsf(det) < 1e-12f) {
        // Degenerate: fall back to the plain 3x3 (better than NaNs everywhere).
        out[0] = a; out[4] = b; out[8]  = c;
        out[1] = d; out[5] = e; out[9]  = f;
        out[2] = g; out[6] = h; out[10] = i;
        return;
    }
    float id_ = 1.0f / det;
    // inverse (adjugate/det) ...
    float i00 = (e * i - f * h) * id_, i01 = (c * h - b * i) * id_, i02 = (b * f - c * e) * id_;
    float i10 = (f * g - d * i) * id_, i11 = (a * i - c * g) * id_, i12 = (c * d - a * f) * id_;
    float i20 = (d * h - e * g) * id_, i21 = (b * g - a * h) * id_, i22 = (a * e - b * d) * id_;
    // ... transposed, written column-major: out[col*4+row] = inv[col][row]
    out[0] = i00; out[4] = i10; out[8]  = i20;
    out[1] = i01; out[5] = i11; out[9]  = i21;
    out[2] = i02; out[6] = i12; out[10] = i22;
}

static void mat_to_simd(const float* m, simd_float4x4* out) { memcpy(out, m, 16 * sizeof(float)); }

// -------------------------------------------------------------------------------- MSL src -----
// ObjC has no raw string literals, so the source is assembled from per-line literals. The
// smooth/flat pair is one template instantiated twice by substituting $S (name suffix) and
// $Q (interpolation qualifier) — GL_FLAT differs from GL_SMOOTH only in that qualifier.

static NSString* const kGlbShaderPrelude =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "constant bool gLighting    [[function_constant(0)]];\n"
     "constant uint gLightMask   [[function_constant(1)]];\n"
     "constant bool gTexture     [[function_constant(2)]];\n"
     "constant bool gNormalize   [[function_constant(3)]];\n"
     "constant bool gColorMat    [[function_constant(4)]];\n"
     "constant bool gTwoSided    [[function_constant(5)]];\n"
     "constant bool gLocalViewer [[function_constant(6)]];\n"
     "struct Vtx {\n"
     "    packed_float3 pos;\n"
     "    packed_float3 nrm;\n"
     "    packed_float4 col;\n"
     "    packed_float2 uv;\n"
     "};\n"
     "struct U {\n"
     "    float4x4 mvp;\n"
     "    float4x4 mv;\n"
     "    float4x4 nrm;\n"
     "    float4 lpos[8];\n"
     "    float4 lamb[8];\n"
     "    float4 ldif[8];\n"
     "    float4 lspc[8];\n"
     "    float4 mAmb;\n"
     "    float4 mDif;\n"
     "    float4 mSpc;\n"
     "    float4 mEmi;\n"
     "    float4 sceneAmb;\n"
     "    float4 misc;\n"
     "};\n";

static NSString* const kGlbShaderTemplate =
    @"struct VOut$S {\n"
     "    float4 position [[position]];\n"
     "    float  psize [[point_size]];\n"
     "    $Q float3 epos;\n"
     "    $Q float3 nrm;\n"
     "    $Q float4 col;\n"
     "    $Q float2 uv;\n"
     "};\n"
     "vertex VOut$S vs$S(uint vid [[vertex_id]],\n"
     "                   const device Vtx* vb [[buffer(0)]],\n"
     "                   constant U& u [[buffer(1)]]) {\n"
     "    VOut$S o;\n"
     "    float4 p = float4(float3(vb[vid].pos), 1.0);\n"
     "    o.position = u.mvp * p;\n"
     "    o.epos = (u.mv * p).xyz;\n"
     "    float3 n = (u.nrm * float4(float3(vb[vid].nrm), 0.0)).xyz;\n"
     "    if (gNormalize) { float l = length(n); n = (l > 1e-8) ? (n / l) : n; }\n"
     "    o.nrm = n;\n"
     "    o.col = float4(vb[vid].col);\n"
     "    o.uv  = float2(vb[vid].uv);\n"
     "    o.psize = u.misc.y;\n"
     "    return o;\n"
     "}\n"
     "fragment float4 fs$S(VOut$S in [[stage_in]],\n"
     "                     constant U& u [[buffer(0)]],\n"
     "                     texture2d<float> tex [[texture(0)]],\n"
     "                     sampler smp [[sampler(0)]],\n"
     "                     bool front [[front_facing]]) {\n"
     "    float4 outc;\n"
     "    if (gLighting) {\n"
     "        float3 N = in.nrm;\n"
     "        float nl = length(N);\n"
     "        N = (nl > 1e-8) ? (N / nl) : float3(0.0, 0.0, 1.0);\n"
     "        if (gTwoSided && !front) N = -N;\n"
     "        float3 V = gLocalViewer ? normalize(-in.epos) : float3(0.0, 0.0, 1.0);\n"
     "        float4 dif = gColorMat ? in.col : u.mDif;\n"
     "        float4 amb = gColorMat ? in.col : u.mAmb;\n"
     "        float3 c = u.mEmi.rgb + u.sceneAmb.rgb * amb.rgb;\n"
     "        for (uint i = 0u; i < 8u; ++i) {\n"
     "            if ((gLightMask & (1u << i)) == 0u) continue;\n"
     "            float3 L = (u.lpos[i].w == 0.0)\n"
     "                     ? normalize(u.lpos[i].xyz)\n"
     "                     : normalize(u.lpos[i].xyz - in.epos);\n"
     "            c += u.lamb[i].rgb * amb.rgb;\n"
     "            float ndl = dot(N, L);\n"
     "            if (ndl > 0.0) {\n"
     "                c += u.ldif[i].rgb * dif.rgb * ndl;\n"
     "                float3 H = normalize(L + V);\n"
     "                float ndh = dot(N, H);\n"
     "                if (ndh > 0.0 && u.misc.x > 0.0)\n"
     "                    c += u.lspc[i].rgb * u.mSpc.rgb * pow(ndh, u.misc.x);\n"
     "            }\n"
     "        }\n"
     "        outc = float4(c, dif.a);\n"
     "    } else {\n"
     "        outc = in.col;\n"
     "    }\n"
     "    if (gTexture) outc *= tex.sample(smp, in.uv);\n"
     "    return float4(saturate(outc.rgb), saturate(outc.a));\n"
     "}\n";

static NSString* glb_shader_source(void) {
    NSString* smooth = [[kGlbShaderTemplate
        stringByReplacingOccurrencesOfString:@"$S" withString:@"_smooth"]
        stringByReplacingOccurrencesOfString:@"$Q" withString:@""];
    NSString* flat = [[kGlbShaderTemplate
        stringByReplacingOccurrencesOfString:@"$S" withString:@"_flat"]
        stringByReplacingOccurrencesOfString:@"$Q" withString:@"[[flat]]"];
    return [NSString stringWithFormat:@"%@%@%@", kGlbShaderPrelude, smooth, flat];
}

// --------------------------------------------------------------------------- enum mapping -----
static MTLCompareFunction map_compare(uint32_t f) {
    switch (f) {
        case GLB_NEVER:    return MTLCompareFunctionNever;
        case GLB_LESS:     return MTLCompareFunctionLess;
        case GLB_EQUAL:    return MTLCompareFunctionEqual;
        case GLB_LEQUAL:   return MTLCompareFunctionLessEqual;
        case GLB_GREATER:  return MTLCompareFunctionGreater;
        case GLB_NOTEQUAL: return MTLCompareFunctionNotEqual;
        case GLB_GEQUAL:   return MTLCompareFunctionGreaterEqual;
        default:           return MTLCompareFunctionAlways;
    }
}

static MTLBlendFactor map_blend(uint32_t f) {
    switch (f) {
        case GLB_ZERO:                   return MTLBlendFactorZero;
        case GLB_ONE:                    return MTLBlendFactorOne;
        case GLB_SRC_COLOR:              return MTLBlendFactorSourceColor;
        case GLB_ONE_MINUS_SRC_COLOR:    return MTLBlendFactorOneMinusSourceColor;
        case GLB_SRC_ALPHA:              return MTLBlendFactorSourceAlpha;
        case GLB_ONE_MINUS_SRC_ALPHA:    return MTLBlendFactorOneMinusSourceAlpha;
        case GLB_DST_ALPHA:              return MTLBlendFactorDestinationAlpha;
        case GLB_ONE_MINUS_DST_ALPHA:    return MTLBlendFactorOneMinusDestinationAlpha;
        case GLB_DST_COLOR:              return MTLBlendFactorDestinationColor;
        case GLB_ONE_MINUS_DST_COLOR:    return MTLBlendFactorOneMinusDestinationColor;
        case GLB_SRC_ALPHA_SATURATE:     return MTLBlendFactorSourceAlphaSaturated;
        default:                         return MTLBlendFactorOne;
    }
}

// -------------------------------------------------------------------------- state caches ------
static id<MTLRenderPipelineState> pipeline_for_key(uint64_t key) {
    NSNumber* k = @(key);
    id<MTLRenderPipelineState> ps = G.pipeCache[k];
    if (ps) return ps;

    BOOL flat        = (key & 1ull) != 0;
    BOOL lighting    = (key & 2ull) != 0;
    BOOL texture     = (key & 4ull) != 0;
    BOOL normalize_  = (key & 8ull) != 0;
    BOOL colorMat    = (key & 16ull) != 0;
    BOOL twoSided    = (key & 32ull) != 0;
    BOOL localViewer = (key & 64ull) != 0;
    uint32_t lmask   = (uint32_t)((key >> 8) & 0xFFull);
    BOOL blend       = (key & (1ull << 16)) != 0;
    uint32_t src     = (uint32_t)((key >> 20) & 0x1Full);
    uint32_t dst     = (uint32_t)((key >> 25) & 0x1Full);

    MTLFunctionConstantValues* fc = [MTLFunctionConstantValues new];
    [fc setConstantValue:&lighting    type:MTLDataTypeBool atIndex:0];
    [fc setConstantValue:&lmask       type:MTLDataTypeUInt atIndex:1];
    [fc setConstantValue:&texture     type:MTLDataTypeBool atIndex:2];
    [fc setConstantValue:&normalize_  type:MTLDataTypeBool atIndex:3];
    [fc setConstantValue:&colorMat    type:MTLDataTypeBool atIndex:4];
    [fc setConstantValue:&twoSided    type:MTLDataTypeBool atIndex:5];
    [fc setConstantValue:&localViewer type:MTLDataTypeBool atIndex:6];

    NSError* err = nil;
    NSString* vn = flat ? @"vs_flat" : @"vs_smooth";
    NSString* fn = flat ? @"fs_flat" : @"fs_smooth";
    id<MTLFunction> vf = [G.lib newFunctionWithName:vn constantValues:fc error:&err];
    if (!vf) { NSLog(@"glbridge: vertex fn %@: %@", vn, err); return nil; }
    id<MTLFunction> ff = [G.lib newFunctionWithName:fn constantValues:fc error:&err];
    if (!ff) { NSLog(@"glbridge: fragment fn %@: %@", fn, err); return nil; }

    MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = vf;
    d.fragmentFunction = ff;
    d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    if (blend) {
        d.colorAttachments[0].blendingEnabled = YES;
        d.colorAttachments[0].sourceRGBBlendFactor = (MTLBlendFactor)src;
        d.colorAttachments[0].sourceAlphaBlendFactor = (MTLBlendFactor)src;
        d.colorAttachments[0].destinationRGBBlendFactor = (MTLBlendFactor)dst;
        d.colorAttachments[0].destinationAlphaBlendFactor = (MTLBlendFactor)dst;
    }
    ps = [G.dev newRenderPipelineStateWithDescriptor:d error:&err];
    if (!ps) { NSLog(@"glbridge: pipeline: %@", err); return nil; }
    G.pipeCache[k] = ps;
    return ps;
}

static id<MTLDepthStencilState> depth_state_for_key(uint64_t key) {
    NSNumber* k = @(key);
    id<MTLDepthStencilState> ds = G.depthCache[k];
    if (ds) return ds;
    MTLDepthStencilDescriptor* d = [MTLDepthStencilDescriptor new];
    d.depthCompareFunction = (MTLCompareFunction)((key >> 1) & 0xFull);
    d.depthWriteEnabled = (key & 1ull) != 0;
    ds = [G.dev newDepthStencilStateWithDescriptor:d];
    G.depthCache[k] = ds;
    return ds;
}

static MTLSamplerMinMagFilter map_filter(uint32_t f) {
    // GL mipmap filters collapse to their base filter here (no mip chains generated).
    switch (f) {
        case GLB_NEAREST: case 0x2700 /*NEAREST_MIPMAP_NEAREST*/: case 0x2702:
            return MTLSamplerMinMagFilterNearest;
        default: return MTLSamplerMinMagFilterLinear;
    }
}

static MTLSamplerAddressMode map_wrap(uint32_t w) {
    return (w == GLB_REPEAT) ? MTLSamplerAddressModeRepeat : MTLSamplerAddressModeClampToEdge;
}

static id<MTLSamplerState> sampler_for_state(void) {
    uint64_t key = ((uint64_t)G.texMin) | ((uint64_t)G.texMag << 16) |
                   ((uint64_t)G.texWrapS << 32) | ((uint64_t)G.texWrapT << 48);
    NSNumber* k = @(key);
    id<MTLSamplerState> s = G.samplerCache[k];
    if (s) return s;
    MTLSamplerDescriptor* d = [MTLSamplerDescriptor new];
    d.minFilter = map_filter(G.texMin);
    d.magFilter = map_filter(G.texMag);
    d.sAddressMode = map_wrap(G.texWrapS);
    d.tAddressMode = map_wrap(G.texWrapT);
    s = [G.dev newSamplerStateWithDescriptor:d];
    G.samplerCache[k] = s;
    return s;
}

// ------------------------------------------------------------------------------- ring ---------
static GlbAlloc ring_alloc(NSUInteger bytes) {
    GlbAlloc a = {nil, 0, NULL};
    NSUInteger want = (bytes + (GLB_ALIGN - 1)) & ~(NSUInteger)(GLB_ALIGN - 1);
    if (G.ringOff + want <= GLB_RING_BYTES) {
        a.buf = G.ring[G.slot];
        a.off = G.ringOff;
        a.ptr = (uint8_t*)a.buf.contents + G.ringOff;
        G.ringOff += want;
        return a;
    }
    // Cold path: this frame is unusually fat. Allocate a one-off buffer and keep it alive until
    // the frame's command buffer completes.
    id<MTLBuffer> b = [G.dev newBufferWithLength:want options:MTLResourceStorageModeShared];
    if (!b) return a;
    [G.overflow[G.slot] addObject:b];
    a.buf = b; a.off = 0; a.ptr = b.contents;
    return a;
}

// ------------------------------------------------------------------------- frame plumbing -----
static void end_encoder(void) {
    if (G.enc) { [G.enc endEncoding]; G.enc = nil; }
}

static void begin_frame_if_needed(void) {
    if (G.frameOpen) return;
    dispatch_semaphore_wait(G.sem, DISPATCH_TIME_FOREVER);
    G.slot = (int)(G.frameCounter % GLB_RING_SLOTS);
    G.ringOff = 0;
    [G.overflow[G.slot] removeAllObjects];
    G.cmdBuf = [G.queue commandBuffer];
    G.cmdBuf.label = @"glbridge frame";
    G.frameOpen = 1;
    G.uniformsDirty = 1;
    G.uniformsAlloc.buf = nil;
    G.drawsThisFrame = G.vertsThisFrame = 0;
}

static void open_encoder(void) {
    if (G.enc) return;
    begin_frame_if_needed();

    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = G.color;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    if (G.pendingClearMask & GLB_COLOR_BUFFER_BIT) {
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(G.pendingClear[0], G.pendingClear[1],
                                                             G.pendingClear[2], G.pendingClear[3]);
    } else {
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    }
    rp.depthAttachment.texture = G.depth;
    rp.depthAttachment.storeAction = MTLStoreActionStore;
    if (G.pendingClearMask & GLB_DEPTH_BUFFER_BIT) {
        rp.depthAttachment.loadAction = MTLLoadActionClear;
        rp.depthAttachment.clearDepth = G.pendingClearDepth;
    } else {
        rp.depthAttachment.loadAction = MTLLoadActionLoad;
    }
    G.pendingClearMask = 0;

    G.enc = [G.cmdBuf renderCommandEncoderWithDescriptor:rp];
    // Everything must be re-set on a fresh encoder.
    G.curPipeKey = G.curDepthKey = UINT64_MAX;
    G.curCull = G.curWinding = -1;
    G.boundVB = G.boundVU = G.boundFU = nil;
    G.boundTex = nil;
    G.boundSmp = nil;
    G.viewportDirty = 1;
    G.uniformsAlloc.buf = nil;   // uniforms live in the ring; re-bind on the new encoder
    G.uniformsDirty = 1;
}

static void commit_frame(void) {
    end_encoder();
    if (!G.frameOpen) return;
    dispatch_semaphore_t sem = G.sem;
    [G.cmdBuf addCompletedHandler:^(id<MTLCommandBuffer> cb) { (void)cb; dispatch_semaphore_signal(sem); }];
    [G.cmdBuf commit];
    G.lastCmdBuf = G.cmdBuf;
    G.cmdBuf = nil;
    G.frameOpen = 0;
    G.drawsLast = G.drawsThisFrame;
    G.vertsLast = G.vertsThisFrame;
    G.frameCounter++;
}

static void drain(void) {
    if (G.frameOpen) commit_frame();
    if (G.lastCmdBuf) [G.lastCmdBuf waitUntilCompleted];
}

// ------------------------------------------------------------------------ uniform upload ------
// GL clip space has z in [-1,1]; Metal wants [0,1]. Y needs NO flip: Metal's viewport transform
// already maps NDC y=+1 to framebuffer row 0, which is exactly where GL's "top" belongs in a
// top-left-origin texture. Column-major: z' = 0.5*z + 0.5*w.
static const float kClipCorrect[16] = {
    1, 0, 0,    0,
    0, 1, 0,    0,
    0, 0, 0.5f, 0,
    0, 0, 0.5f, 1,
};

static GlbAlloc upload_uniforms(void) {
    if (!G.uniformsDirty && G.uniformsAlloc.buf) return G.uniformsAlloc;

    GlbAlloc a = ring_alloc(sizeof(GlbUniforms));
    if (!a.ptr) return a;
    GlbUniforms* u = (GlbUniforms*)a.ptr;

    const float* mv = G.mv[G.mvTop];
    const float* pr = G.pr[G.prTop];
    float pm[16], mvp[16], nm[16];
    mat_mul(pm, kClipCorrect, pr);
    mat_mul(mvp, pm, mv);
    normal_matrix(mv, nm);

    mat_to_simd(mvp, &u->mvp);
    mat_to_simd(mv, &u->mv);
    mat_to_simd(nm, &u->nrm);
    for (int i = 0; i < GLB_MAX_LIGHTS; i++) {
        u->lpos[i] = simd_make_float4(G.lpos[i][0], G.lpos[i][1], G.lpos[i][2], G.lpos[i][3]);
        u->lamb[i] = simd_make_float4(G.lamb[i][0], G.lamb[i][1], G.lamb[i][2], G.lamb[i][3]);
        u->ldif[i] = simd_make_float4(G.ldif[i][0], G.ldif[i][1], G.ldif[i][2], G.ldif[i][3]);
        u->lspc[i] = simd_make_float4(G.lspc[i][0], G.lspc[i][1], G.lspc[i][2], G.lspc[i][3]);
    }
    u->mAmb     = simd_make_float4(G.mAmb[0], G.mAmb[1], G.mAmb[2], G.mAmb[3]);
    u->mDif     = simd_make_float4(G.mDif[0], G.mDif[1], G.mDif[2], G.mDif[3]);
    u->mSpc     = simd_make_float4(G.mSpc[0], G.mSpc[1], G.mSpc[2], G.mSpc[3]);
    u->mEmi     = simd_make_float4(G.mEmi[0], G.mEmi[1], G.mEmi[2], G.mEmi[3]);
    u->sceneAmb = simd_make_float4(G.sceneAmb[0], G.sceneAmb[1], G.sceneAmb[2], G.sceneAmb[3]);
    u->misc     = simd_make_float4(G.shininess, G.pointSize, 0.0f, 0.0f);

    G.uniformsAlloc = a;
    G.uniformsDirty = 0;
    return a;
}

// ------------------------------------------------------------------------------ the draw ------
static uint64_t pipe_key(void) {
    uint64_t k = 0;
    if (G.shadeModel == GLB_FLAT) k |= 1ull;
    if (G.enLighting)             k |= 2ull;
    if (G.enTexture && G.boundTexId) k |= 4ull;
    if (G.enNormalize)            k |= 8ull;
    if (G.enColorMat)             k |= 16ull;
    if (G.twoSided)               k |= 32ull;
    if (G.localViewer)            k |= 64ull;
    k |= ((uint64_t)(G.enLighting ? G.lightEnabled : 0u) & 0xFFull) << 8;
    if (G.enBlend) {
        k |= 1ull << 16;
        k |= ((uint64_t)map_blend(G.blendSrc) & 0x1Full) << 20;
        k |= ((uint64_t)map_blend(G.blendDst) & 0x1Full) << 25;
    }
    return k;
}

static uint64_t depth_key(void) {
    if (!G.enDepth) return (uint64_t)MTLCompareFunctionAlways << 1;   // no test, no writes
    uint64_t k = (uint64_t)map_compare(G.depthFunc) << 1;
    if (G.depthWrite) k |= 1ull;
    return k;
}

static void apply_viewport(void) {
    if (!G.viewportDirty) return;
    // GL viewport is bottom-left origin; our target is top-left origin (we y-flip in the MVP).
    MTLViewport vp;
    vp.originX = (double)G.vpX;
    vp.originY = (double)(G.fbH - (G.vpY + G.vpH));
    vp.width   = (double)G.vpW;
    vp.height  = (double)G.vpH;
    vp.znear = 0.0; vp.zfar = 1.0;
    if (vp.width <= 0) vp.width = 1;
    if (vp.height <= 0) vp.height = 1;
    if (vp.originX < 0) vp.originX = 0;
    if (vp.originY < 0) vp.originY = 0;
    if (vp.originX + vp.width  > G.fbW) vp.width  = G.fbW - vp.originX;
    if (vp.originY + vp.height > G.fbH) vp.height = G.fbH - vp.originY;
    [G.enc setViewport:vp];
    G.viewportDirty = 0;
}

static void emit_draw(MTLPrimitiveType type, GlbAlloc verts, uint32_t count) {
    if (count == 0 || !verts.ptr) return;
    open_encoder();
    if (!G.enc) return;
    apply_viewport();

    uint64_t pk = pipe_key();
    if (pk != G.curPipeKey) {
        id<MTLRenderPipelineState> ps = pipeline_for_key(pk);
        if (!ps) return;
        [G.enc setRenderPipelineState:ps];
        G.curPipeKey = pk;
    }
    uint64_t dk = depth_key();
    if (dk != G.curDepthKey) {
        [G.enc setDepthStencilState:depth_state_for_key(dk)];
        G.curDepthKey = dk;
    }

    int cull = MTLCullModeNone, winding;
    if (G.enCull && (type == MTLPrimitiveTypeTriangle || type == MTLPrimitiveTypeTriangleStrip)) {
        if (G.cullFace == GLB_FRONT_AND_BACK) return;           // nothing survives
        cull = (G.cullFace == GLB_FRONT) ? MTLCullModeFront : MTLCullModeBack;
    }
    // Metal classifies facing in NDC (y up), not in the y-down framebuffer, and we do not flip Y
    // in the clip correction — so GL's winding carries over unchanged. Verified empirically by
    // test_backface_cull() in test_glbridge.m; getting this backwards silently shows you the
    // *inside* of every closed model (and hence unlit, near-black geometry).
    winding = (G.frontFace == GLB_CCW) ? MTLWindingCounterClockwise : MTLWindingClockwise;
    if (cull != G.curCull)       { [G.enc setCullMode:(MTLCullMode)cull]; G.curCull = cull; }
    if (winding != G.curWinding) { [G.enc setFrontFacingWinding:(MTLWinding)winding]; G.curWinding = winding; }

    GlbAlloc u = upload_uniforms();
    if (!u.ptr) return;

    if (G.boundVB != verts.buf) {
        [G.enc setVertexBuffer:verts.buf offset:verts.off atIndex:0];
        G.boundVB = verts.buf;
    } else {
        [G.enc setVertexBufferOffset:verts.off atIndex:0];
    }
    if (G.boundVU != u.buf) {
        [G.enc setVertexBuffer:u.buf offset:u.off atIndex:1];
        G.boundVU = u.buf;
    } else {
        [G.enc setVertexBufferOffset:u.off atIndex:1];
    }
    if (G.boundFU != u.buf) {
        [G.enc setFragmentBuffer:u.buf offset:u.off atIndex:0];
        G.boundFU = u.buf;
    } else {
        [G.enc setFragmentBufferOffset:u.off atIndex:0];
    }

    id<MTLTexture> tex = G.white;
    if (G.enTexture && G.boundTexId) {
        id<MTLTexture> t = G.textures[@(G.boundTexId)];
        if (t) tex = t;
    }
    if (tex != G.boundTex) { [G.enc setFragmentTexture:tex atIndex:0]; G.boundTex = tex; }
    id<MTLSamplerState> smp = sampler_for_state();
    if (smp != G.boundSmp) { [G.enc setFragmentSamplerState:smp atIndex:0]; G.boundSmp = smp; }

    [G.enc drawPrimitives:type vertexStart:0 vertexCount:count];
    G.drawsThisFrame++;
    G.vertsThisFrame += count;
}

// ============================================================================ public: init =====
static void reset_gl_state(void) {
    G.matrixMode = GLB_MODELVIEW;
    G.mvTop = G.prTop = G.txTop = 0;
    mat_identity(G.mv[0]); mat_identity(G.pr[0]); mat_identity(G.tx[0]);

    G.enDepth = G.enLighting = G.enCull = G.enNormalize = G.enTexture = G.enBlend = 0;
    G.enColorMat = 0;
    G.lightEnabled = 0;
    G.depthFunc = GLB_LESS; G.depthWrite = 1;
    G.cullFace = GLB_BACK; G.frontFace = GLB_CCW; G.shadeModel = GLB_SMOOTH;
    G.blendSrc = GLB_ONE; G.blendDst = GLB_ZERO;
    G.pointSize = 1.0f;
    G.colorMatMode = GLB_AMBIENT_AND_DIFFUSE;

    for (int i = 0; i < GLB_MAX_LIGHTS; i++) {
        G.lpos[i][0] = 0; G.lpos[i][1] = 0; G.lpos[i][2] = 1; G.lpos[i][3] = 0;
        G.lamb[i][0] = G.lamb[i][1] = G.lamb[i][2] = 0; G.lamb[i][3] = 1;
        float d = (i == 0) ? 1.0f : 0.0f;
        G.ldif[i][0] = G.ldif[i][1] = G.ldif[i][2] = d; G.ldif[i][3] = 1;
        G.lspc[i][0] = G.lspc[i][1] = G.lspc[i][2] = d; G.lspc[i][3] = 1;
    }
    G.mAmb[0] = G.mAmb[1] = G.mAmb[2] = 0.2f; G.mAmb[3] = 1.0f;
    G.mDif[0] = G.mDif[1] = G.mDif[2] = 0.8f; G.mDif[3] = 1.0f;
    G.mSpc[0] = G.mSpc[1] = G.mSpc[2] = 0.0f; G.mSpc[3] = 1.0f;
    G.mEmi[0] = G.mEmi[1] = G.mEmi[2] = 0.0f; G.mEmi[3] = 1.0f;
    G.shininess = 0.0f;
    G.sceneAmb[0] = G.sceneAmb[1] = G.sceneAmb[2] = 0.2f; G.sceneAmb[3] = 1.0f;
    G.twoSided = 0; G.localViewer = 0;

    G.boundTexId = 0;
    G.texMin = GLB_LINEAR; G.texMag = GLB_LINEAR;
    G.texWrapS = GLB_REPEAT; G.texWrapT = GLB_REPEAT;

    G.inBegin = 0; G.imCount = 0;
    G.curNormal[0] = 0; G.curNormal[1] = 0; G.curNormal[2] = 1;
    G.curColor[0] = G.curColor[1] = G.curColor[2] = G.curColor[3] = 1;
    G.curUV[0] = G.curUV[1] = 0;

    G.clearColor[0] = G.clearColor[1] = G.clearColor[2] = 0; G.clearColor[3] = 1;
    G.clearDepth = 1.0f;
    G.uniformsDirty = 1;
}

static int alloc_targets(int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    MTLTextureDescriptor* cd =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:(NSUInteger)w
                                                          height:(NSUInteger)h
                                                       mipmapped:NO];
    cd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    cd.storageMode = MTLStorageModeShared;
    id<MTLTexture> color = [G.dev newTextureWithDescriptor:cd];
    if (!color) return -ENOMEM;

    MTLTextureDescriptor* dd =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                           width:(NSUInteger)w
                                                          height:(NSUInteger)h
                                                       mipmapped:NO];
    dd.usage = MTLTextureUsageRenderTarget;
    dd.storageMode = MTLStorageModePrivate;
    id<MTLTexture> depth = [G.dev newTextureWithDescriptor:dd];
    if (!depth) return -ENOMEM;

    G.color = color;
    G.depth = depth;
    G.fbW = w; G.fbH = h;
    G.vpX = 0; G.vpY = 0; G.vpW = w; G.vpH = h;
    G.viewportDirty = 1;
    return 0;
}

int glb_init(void* mtl_device_or_null) {
    if (G.inited) return 0;
    @autoreleasepool {
        G.dev = mtl_device_or_null ? (__bridge id<MTLDevice>)mtl_device_or_null
                                   : MTLCreateSystemDefaultDevice();
        if (!G.dev) return -ENODEV;
        G.queue = [G.dev newCommandQueue];
        if (!G.queue) return -ENOMEM;
        G.queue.label = @"glbridge";

        NSError* err = nil;
        G.lib = [G.dev newLibraryWithSource:glb_shader_source() options:nil error:&err];
        if (!G.lib) { NSLog(@"glbridge: shader compile failed: %@", err); return -EINVAL; }

        G.pipeCache    = [NSMutableDictionary new];
        G.depthCache   = [NSMutableDictionary new];
        G.samplerCache = [NSMutableDictionary new];
        G.textures     = [NSMutableDictionary new];

        for (int i = 0; i < GLB_RING_SLOTS; i++) {
            G.ring[i] = [G.dev newBufferWithLength:GLB_RING_BYTES
                                           options:MTLResourceStorageModeShared];
            if (!G.ring[i]) return -ENOMEM;
            G.ring[i].label = [NSString stringWithFormat:@"glbridge ring %d", i];
            G.overflow[i] = [NSMutableArray new];
        }
        G.sem = dispatch_semaphore_create(GLB_RING_SLOTS);

        // 1x1 opaque white, bound whenever texturing is off so the fragment shader always has a
        // valid texture argument.
        MTLTextureDescriptor* wd =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                               width:1 height:1 mipmapped:NO];
        wd.usage = MTLTextureUsageShaderRead;
        wd.storageMode = MTLStorageModeShared;
        G.white = [G.dev newTextureWithDescriptor:wd];
        uint8_t px[4] = {255, 255, 255, 255};
        [G.white replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:px bytesPerRow:4];

        G.imCap = 4096;
        G.imVerts = (GlbVertex*)malloc(sizeof(GlbVertex) * G.imCap);
        if (!G.imVerts) return -ENOMEM;

        G.listCap = 64;
        G.lists = (GlbList*)calloc(G.listCap, sizeof(GlbList));
        if (!G.lists) return -ENOMEM;
        G.listCount = 0;

        reset_gl_state();
        int rc = alloc_targets(640, 480);
        if (rc) return rc;

        G.frameCounter = 0;
        G.inited = 1;
    }
    return 0;
}

int glb_resize(int w, int h) {
    if (!G.inited) return -EINVAL;
    if (w == G.fbW && h == G.fbH) return 0;
    @autoreleasepool {
        drain();
        return alloc_targets(w, h);
    }
}

void glb_framebuffer_size(int* w, int* h) {
    if (w) *w = G.fbW;
    if (h) *h = G.fbH;
}

void glb_destroy(void) {
    if (!G.inited) return;
    @autoreleasepool {
        drain();
        for (int i = 0; i < GLB_RING_SLOTS; i++) { G.ring[i] = nil; G.overflow[i] = nil; }
        G.color = nil; G.depth = nil; G.white = nil;
        G.pipeCache = nil; G.depthCache = nil; G.samplerCache = nil; G.textures = nil;
        G.lib = nil; G.queue = nil; G.dev = nil;
        G.lastCmdBuf = nil; G.cmdBuf = nil; G.enc = nil;
        G.sem = nil;
        free(G.imVerts); G.imVerts = NULL; G.imCap = G.imCount = 0;
        for (uint32_t i = 0; i < G.listCount; i++) free(G.lists[i].cmds);
        free(G.lists); G.lists = NULL; G.listCount = G.listCap = 0;
        G.inited = 0;
        G.frameOpen = 0;
    }
}

void* glb_metal_device(void)  { return (__bridge void*)G.dev; }
void* glb_metal_queue(void)   { return (__bridge void*)G.queue; }
void* glb_frame_texture(void) { return (__bridge void*)G.color; }

int glb_readback(uint8_t* rgba, int w, int h) {
    if (!G.inited || !rgba) return -EINVAL;
    @autoreleasepool {
        drain();
        int cw = (w < G.fbW) ? w : G.fbW;
        int ch = (h < G.fbH) ? h : G.fbH;
        if (cw <= 0 || ch <= 0) return -EINVAL;
        if (cw == w) {
            [G.color getBytes:rgba
                  bytesPerRow:(NSUInteger)w * 4
                   fromRegion:MTLRegionMake2D(0, 0, (NSUInteger)cw, (NSUInteger)ch)
                  mipmapLevel:0];
        } else {
            for (int y = 0; y < ch; y++) {
                [G.color getBytes:rgba + (size_t)y * (size_t)w * 4
                      bytesPerRow:(NSUInteger)cw * 4
                       fromRegion:MTLRegionMake2D(0, (NSUInteger)y, (NSUInteger)cw, 1)
                      mipmapLevel:0];
            }
        }
    }
    return 0;
}

// ======================================================================= display lists =========
static GlbList* list_get(uint32_t id) {
    if (id == 0 || id > G.listCount) return NULL;
    GlbList* l = &G.lists[id - 1];
    return l->used ? l : NULL;
}

static void list_record(const GlbCmd* c) {
    GlbList* l = &G.lists[G.recording - 1];
    if (l->count == l->cap) {
        uint32_t nc = l->cap ? l->cap * 2 : 256;
        GlbCmd* n = (GlbCmd*)realloc(l->cmds, sizeof(GlbCmd) * nc);
        if (!n) return;
        l->cmds = n; l->cap = nc;
    }
    l->cmds[l->count++] = *c;
}

// Entered by every recordable entry point. Returns 1 if the caller should stop (pure record).
#define REC0(OPC)                                                                       \
    if (G.recording) { GlbCmd c_; memset(&c_, 0, sizeof c_); c_.op = (OPC);              \
        list_record(&c_); if (!G.recExec) return; }
#define RECF(OPC, A, B, ...)                                                            \
    if (G.recording) { GlbCmd c_; memset(&c_, 0, sizeof c_); c_.op = (OPC);              \
        c_.a = (A); c_.b = (B); float tmp_[] = {__VA_ARGS__};                            \
        memcpy(c_.f, tmp_, sizeof tmp_); list_record(&c_); if (!G.recExec) return; }
#define RECV(OPC, A, B, PTR, N)                                                          \
    if (G.recording) { GlbCmd c_; memset(&c_, 0, sizeof c_); c_.op = (OPC);              \
        c_.a = (A); c_.b = (B); if (PTR) memcpy(c_.f, (PTR), sizeof(float) * (N));       \
        list_record(&c_); if (!G.recExec) return; }

uint32_t glb_gen_lists(int n) {
    if (!G.inited || n <= 0) return 0;
    uint32_t base = G.listCount + 1;
    uint32_t need = G.listCount + (uint32_t)n;
    if (need > G.listCap) {
        uint32_t nc = G.listCap ? G.listCap : 64;
        while (nc < need) nc *= 2;
        GlbList* nl = (GlbList*)realloc(G.lists, sizeof(GlbList) * nc);
        if (!nl) return 0;
        memset(nl + G.listCap, 0, sizeof(GlbList) * (nc - G.listCap));
        G.lists = nl; G.listCap = nc;
    }
    for (int i = 0; i < n; i++) {
        GlbList* l = &G.lists[G.listCount + (uint32_t)i];
        l->count = 0;
        l->used = 1;
    }
    G.listCount = need;
    return base;
}

void glb_new_list(uint32_t id, uint32_t mode) {
    if (!G.inited || id == 0) return;
    if (id > G.listCount) {
        // Client picked its own id (legal in GL). Grow to cover it.
        uint32_t need = id;
        if (need > G.listCap) {
            uint32_t nc = G.listCap ? G.listCap : 64;
            while (nc < need) nc *= 2;
            GlbList* nl = (GlbList*)realloc(G.lists, sizeof(GlbList) * nc);
            if (!nl) return;
            memset(nl + G.listCap, 0, sizeof(GlbList) * (nc - G.listCap));
            G.lists = nl; G.listCap = nc;
        }
        for (uint32_t i = G.listCount; i < need; i++) { G.lists[i].count = 0; G.lists[i].used = 1; }
        G.listCount = need;
    }
    G.lists[id - 1].used = 1;
    G.lists[id - 1].count = 0;
    G.recording = id;
    G.recExec = (mode == GLB_COMPILE_AND_EXECUTE);
}

void glb_end_list(void) { G.recording = 0; G.recExec = 0; }

void glb_delete_lists(uint32_t id, int n) {
    for (int i = 0; i < n; i++) {
        GlbList* l = list_get(id + (uint32_t)i);
        if (l) { l->count = 0; l->used = 0; }
    }
}

int glb_is_list(uint32_t id) { return list_get(id) != NULL; }

static void replay(const GlbList* l);

void glb_call_list(uint32_t id) {
    if (G.recording) {
        GlbCmd c; memset(&c, 0, sizeof c); c.op = OP_CALL_LIST; c.a = id;
        list_record(&c);
        if (!G.recExec) return;
    }
    GlbList* l = list_get(id);
    if (!l) return;
    if (G.replayDepth >= GLB_MAX_LIST_DEPTH) return;
    G.replayDepth++;
    replay(l);
    G.replayDepth--;
}

// ======================================================================== matrix stack =========
static void do_matrix_mode(uint32_t m) { G.matrixMode = m; }

static void do_push(void) {
    switch (G.matrixMode) {
        case GLB_PROJECTION:
            if (G.prTop + 1 < GLB_MAX_STACK) { memcpy(G.pr[G.prTop + 1], G.pr[G.prTop], 64); G.prTop++; }
            break;
        case GLB_TEXTURE:
            if (G.txTop + 1 < GLB_MAX_STACK) { memcpy(G.tx[G.txTop + 1], G.tx[G.txTop], 64); G.txTop++; }
            break;
        default:
            if (G.mvTop + 1 < GLB_MAX_STACK) { memcpy(G.mv[G.mvTop + 1], G.mv[G.mvTop], 64); G.mvTop++; }
            break;
    }
}

static void do_pop(void) {
    switch (G.matrixMode) {
        case GLB_PROJECTION: if (G.prTop > 0) G.prTop--; break;
        case GLB_TEXTURE:    if (G.txTop > 0) G.txTop--; break;
        default:             if (G.mvTop > 0) G.mvTop--; break;
    }
    G.uniformsDirty = 1;
}

static void do_translate(float x, float y, float z) {
    float m[16]; mat_identity(m);
    m[12] = x; m[13] = y; m[14] = z;
    mat_post(m);
}

static void do_scale(float x, float y, float z) {
    float m[16]; mat_identity(m);
    m[0] = x; m[5] = y; m[10] = z;
    mat_post(m);
}

static void do_rotate(float deg, float x, float y, float z) {
    float len = sqrtf(x * x + y * y + z * z);
    if (len < 1e-12f) return;
    x /= len; y /= len; z /= len;
    float a = deg * (float)(M_PI / 180.0);
    float c = cosf(a), s = sinf(a), ic = 1.0f - c;
    float m[16]; mat_identity(m);
    m[0] = x * x * ic + c;      m[4] = x * y * ic - z * s;  m[8]  = x * z * ic + y * s;
    m[1] = y * x * ic + z * s;  m[5] = y * y * ic + c;      m[9]  = y * z * ic - x * s;
    m[2] = z * x * ic - y * s;  m[6] = z * y * ic + x * s;  m[10] = z * z * ic + c;
    mat_post(m);
}

static void do_frustum(float l, float r, float b, float t, float n, float f) {
    float m[16]; memset(m, 0, sizeof m);
    m[0]  = 2.0f * n / (r - l);
    m[5]  = 2.0f * n / (t - b);
    m[8]  = (r + l) / (r - l);
    m[9]  = (t + b) / (t - b);
    m[10] = -(f + n) / (f - n);
    m[11] = -1.0f;
    m[14] = -2.0f * f * n / (f - n);
    mat_post(m);
}

static void do_ortho(float l, float r, float b, float t, float n, float f) {
    float m[16]; mat_identity(m);
    m[0]  = 2.0f / (r - l);
    m[5]  = 2.0f / (t - b);
    m[10] = -2.0f / (f - n);
    m[12] = -(r + l) / (r - l);
    m[13] = -(t + b) / (t - b);
    m[14] = -(f + n) / (f - n);
    mat_post(m);
}

void glb_matrix_mode(uint32_t mode) { RECF(OP_MATRIX_MODE, mode, 0, 0.0f); do_matrix_mode(mode); }
void glb_load_identity(void) { REC0(OP_LOAD_IDENTITY); mat_identity(cur_stack_top()); G.uniformsDirty = 1; }
void glb_load_matrix(const float m[16]) {
    RECV(OP_LOAD_MATRIX, 0, 0, m, 16);
    if (m) { memcpy(cur_stack_top(), m, 64); G.uniformsDirty = 1; }
}
void glb_mult_matrix(const float m[16]) { RECV(OP_MULT_MATRIX, 0, 0, m, 16); if (m) mat_post(m); }
void glb_push_matrix(void) { REC0(OP_PUSH); do_push(); }
void glb_pop_matrix(void)  { REC0(OP_POP);  do_pop(); }
void glb_translate(float x, float y, float z) { RECF(OP_TRANSLATE, 0, 0, x, y, z); do_translate(x, y, z); }
void glb_rotate(float a, float x, float y, float z) { RECF(OP_ROTATE, 0, 0, a, x, y, z); do_rotate(a, x, y, z); }
void glb_scale(float x, float y, float z) { RECF(OP_SCALE, 0, 0, x, y, z); do_scale(x, y, z); }
void glb_frustum(float l, float r, float b, float t, float n, float f) {
    RECF(OP_FRUSTUM, 0, 0, l, r, b, t, n, f); do_frustum(l, r, b, t, n, f);
}
void glb_ortho(float l, float r, float b, float t, float n, float f) {
    RECF(OP_ORTHO, 0, 0, l, r, b, t, n, f); do_ortho(l, r, b, t, n, f);
}

void glb_get_matrix(uint32_t mode, float out[16]) {
    if (!out) return;
    const float* s;
    switch (mode) {
        case GLB_PROJECTION: s = G.pr[G.prTop]; break;
        case GLB_TEXTURE:    s = G.tx[G.txTop]; break;
        default:             s = G.mv[G.mvTop]; break;
    }
    memcpy(out, s, 64);
}

// ======================================================================= immediate mode ========
static void do_begin(uint32_t prim) {
    G.inBegin = 1;
    G.prim = prim;
    G.imCount = 0;
}

static void im_push(float x, float y, float z) {
    if (!G.inBegin) return;
    if (G.imCount == G.imCap) {
        uint32_t nc = G.imCap * 2;
        GlbVertex* n = (GlbVertex*)realloc(G.imVerts, sizeof(GlbVertex) * nc);
        if (!n) return;
        G.imVerts = n; G.imCap = nc;
    }
    GlbVertex* v = &G.imVerts[G.imCount++];
    v->pos[0] = x; v->pos[1] = y; v->pos[2] = z;
    memcpy(v->nrm, G.curNormal, sizeof v->nrm);
    memcpy(v->col, G.curColor, sizeof v->col);
    memcpy(v->uv,  G.curUV,    sizeof v->uv);
}

// How many vertices the expansion produces, and which Metal primitive it becomes.
static uint32_t expanded_count(uint32_t prim, uint32_t n, MTLPrimitiveType* type) {
    switch (prim) {
        case GLB_POINTS:         *type = MTLPrimitiveTypePoint;         return n;
        case GLB_LINES:          *type = MTLPrimitiveTypeLine;          return n & ~1u;
        case GLB_LINE_STRIP:     *type = MTLPrimitiveTypeLineStrip;     return n;
        case GLB_LINE_LOOP:      *type = MTLPrimitiveTypeLine;          return (n < 2) ? 0 : n * 2;
        case GLB_TRIANGLES:      *type = MTLPrimitiveTypeTriangle;      return n - (n % 3);
        case GLB_TRIANGLE_STRIP: *type = MTLPrimitiveTypeTriangleStrip; return (n < 3) ? 0 : n;
        case GLB_TRIANGLE_FAN:
        case GLB_POLYGON:        *type = MTLPrimitiveTypeTriangle;      return (n < 3) ? 0 : (n - 2) * 3;
        case GLB_QUADS:          *type = MTLPrimitiveTypeTriangle;      return (n / 4) * 6;
        case GLB_QUAD_STRIP:     *type = MTLPrimitiveTypeTriangle;      return (n < 4) ? 0 : ((n / 2) - 1) * 6;
        default:                 *type = MTLPrimitiveTypeTriangle;      return 0;
    }
}

// Expand the accumulated vertices straight into the (already bump-allocated) ring memory: no
// intermediate malloc, no second scratch copy.
static void expand_into(GlbVertex* dst, uint32_t prim, const GlbVertex* src,
                        uint32_t n, uint32_t out) {
    uint32_t o = 0;
    switch (prim) {
        case GLB_LINE_LOOP:
            for (uint32_t i = 0; i < n; i++) { dst[o++] = src[i]; dst[o++] = src[(i + 1) % n]; }
            break;
        case GLB_TRIANGLE_FAN:
        case GLB_POLYGON:
            for (uint32_t i = 1; i + 1 < n; i++) {
                dst[o++] = src[0]; dst[o++] = src[i]; dst[o++] = src[i + 1];
            }
            break;
        case GLB_QUADS:
            for (uint32_t i = 0; i + 3 < n; i += 4) {
                dst[o++] = src[i]; dst[o++] = src[i + 1]; dst[o++] = src[i + 2];
                dst[o++] = src[i]; dst[o++] = src[i + 2]; dst[o++] = src[i + 3];
            }
            break;
        case GLB_QUAD_STRIP:
            // quad k spans (2k, 2k+1, 2k+3, 2k+2) in polygon order
            for (uint32_t k = 0; 2 * k + 3 < n; k++) {
                const GlbVertex* v0 = &src[2 * k];
                const GlbVertex* v1 = &src[2 * k + 1];
                const GlbVertex* v2 = &src[2 * k + 3];
                const GlbVertex* v3 = &src[2 * k + 2];
                dst[o++] = *v0; dst[o++] = *v1; dst[o++] = *v2;
                dst[o++] = *v0; dst[o++] = *v2; dst[o++] = *v3;
            }
            break;
        default:
            // Pass-through primitives: `out` may be smaller than `n` (a trailing partial triangle
            // or dangling line vertex is dropped, as GL does). Never copy more than was allocated.
            memcpy(dst, src, sizeof(GlbVertex) * ((out < n) ? out : n));
            break;
    }
}

static void do_end(void) {
    if (!G.inBegin) return;
    G.inBegin = 0;
    uint32_t n = G.imCount;
    if (n == 0) return;

    MTLPrimitiveType type = MTLPrimitiveTypeTriangle;
    uint32_t out = expanded_count(G.prim, n, &type);
    if (out == 0) return;

    begin_frame_if_needed();
    GlbAlloc a = ring_alloc(sizeof(GlbVertex) * out);
    if (!a.ptr) return;
    expand_into((GlbVertex*)a.ptr, G.prim, G.imVerts, n, out);
    emit_draw(type, a, out);
}

void glb_begin(uint32_t prim) { RECF(OP_BEGIN, prim, 0, 0.0f); do_begin(prim); }
void glb_vertex3f(float x, float y, float z) { RECF(OP_VERTEX, 0, 0, x, y, z); im_push(x, y, z); }
void glb_vertex2f(float x, float y) { RECF(OP_VERTEX, 0, 0, x, y, 0.0f); im_push(x, y, 0.0f); }
void glb_normal3f(float x, float y, float z) {
    RECF(OP_NORMAL, 0, 0, x, y, z);
    G.curNormal[0] = x; G.curNormal[1] = y; G.curNormal[2] = z;
}
void glb_color4f(float r, float g, float b, float a) {
    RECF(OP_COLOR, 0, 0, r, g, b, a);
    G.curColor[0] = r; G.curColor[1] = g; G.curColor[2] = b; G.curColor[3] = a;
}
void glb_color3f(float r, float g, float b) { glb_color4f(r, g, b, 1.0f); }
void glb_texcoord2f(float s, float t) {
    RECF(OP_TEXCOORD, 0, 0, s, t);
    G.curUV[0] = s; G.curUV[1] = t;
}
void glb_end(void) { REC0(OP_END); do_end(); }

// ============================================================================== state ==========
static void do_enable(uint32_t cap, int on) {
    switch (cap) {
        case GLB_DEPTH_TEST:     G.enDepth = on; break;
        case GLB_LIGHTING:       G.enLighting = on; break;
        case GLB_CULL_FACE:      G.enCull = on; break;
        case GLB_NORMALIZE:      G.enNormalize = on; break;
        case GLB_TEXTURE_2D:     G.enTexture = on; break;
        case GLB_BLEND:          G.enBlend = on; break;
        case GLB_COLOR_MATERIAL: G.enColorMat = on; break;
        default:
            if (cap >= GLB_LIGHT0 && cap < GLB_LIGHT0 + GLB_MAX_LIGHTS) {
                uint32_t bit = 1u << (cap - GLB_LIGHT0);
                if (on) G.lightEnabled |= bit; else G.lightEnabled &= ~bit;
            }
            break;
    }
}

void glb_enable(uint32_t cap)  { RECF(OP_ENABLE, cap, 0, 0.0f);  do_enable(cap, 1); }
void glb_disable(uint32_t cap) { RECF(OP_DISABLE, cap, 0, 0.0f); do_enable(cap, 0); }

int glb_is_enabled(uint32_t cap) {
    switch (cap) {
        case GLB_DEPTH_TEST:     return G.enDepth;
        case GLB_LIGHTING:       return G.enLighting;
        case GLB_CULL_FACE:      return G.enCull;
        case GLB_NORMALIZE:      return G.enNormalize;
        case GLB_TEXTURE_2D:     return G.enTexture;
        case GLB_BLEND:          return G.enBlend;
        case GLB_COLOR_MATERIAL: return G.enColorMat;
        default:
            if (cap >= GLB_LIGHT0 && cap < GLB_LIGHT0 + GLB_MAX_LIGHTS)
                return (G.lightEnabled >> (cap - GLB_LIGHT0)) & 1u;
            return 0;
    }
}

void glb_depth_func(uint32_t f) { RECF(OP_DEPTH_FUNC, f, 0, 0.0f); G.depthFunc = f; }
void glb_depth_mask(int w)      { RECF(OP_DEPTH_MASK, (uint32_t)(w != 0), 0, 0.0f); G.depthWrite = (w != 0); }
void glb_cull_face(uint32_t f)  { RECF(OP_CULL_FACE, f, 0, 0.0f); G.cullFace = f; }
void glb_front_face(uint32_t d) { RECF(OP_FRONT_FACE, d, 0, 0.0f); G.frontFace = d; }
void glb_shade_model(uint32_t m){ RECF(OP_SHADE_MODEL, m, 0, 0.0f); G.shadeModel = m; }
void glb_blend_func(uint32_t s, uint32_t d) { RECF(OP_BLEND_FUNC, s, d, 0.0f); G.blendSrc = s; G.blendDst = d; }
void glb_point_size(float s)    { RECF(OP_POINT_SIZE, 0, 0, s); G.pointSize = s; G.uniformsDirty = 1; }

void glb_viewport(int x, int y, int w, int h) {
    RECF(OP_VIEWPORT, 0, 0, (float)x, (float)y, (float)w, (float)h);
    G.vpX = x; G.vpY = y; G.vpW = w; G.vpH = h;
    G.viewportDirty = 1;
}

// ============================================================================ lighting =========
static void do_light(uint32_t light, uint32_t pname, const float* p) {
    if (!p) return;
    uint32_t i = (light >= GLB_LIGHT0) ? (light - GLB_LIGHT0) : light;
    if (i >= GLB_MAX_LIGHTS) return;
    switch (pname) {
        case GLB_AMBIENT:  memcpy(G.lamb[i], p, 16); break;
        case GLB_DIFFUSE:  memcpy(G.ldif[i], p, 16); break;
        case GLB_SPECULAR: memcpy(G.lspc[i], p, 16); break;
        case GLB_POSITION: {
            // GL transforms the light position by the modelview in force at glLight() time.
            const float* m = G.mv[G.mvTop];
            float x = p[0], y = p[1], z = p[2], w = p[3];
            G.lpos[i][0] = m[0] * x + m[4] * y + m[8]  * z + m[12] * w;
            G.lpos[i][1] = m[1] * x + m[5] * y + m[9]  * z + m[13] * w;
            G.lpos[i][2] = m[2] * x + m[6] * y + m[10] * z + m[14] * w;
            G.lpos[i][3] = m[3] * x + m[7] * y + m[11] * z + m[15] * w;
            break;
        }
        default: return;
    }
    G.uniformsDirty = 1;
}

static void do_material(uint32_t face, uint32_t pname, const float* p) {
    (void)face;   // front/back materials are not tracked separately.
    if (!p) return;
    switch (pname) {
        case GLB_AMBIENT:             memcpy(G.mAmb, p, 16); break;
        case GLB_DIFFUSE:             memcpy(G.mDif, p, 16); break;
        case GLB_AMBIENT_AND_DIFFUSE: memcpy(G.mAmb, p, 16); memcpy(G.mDif, p, 16); break;
        case GLB_SPECULAR:            memcpy(G.mSpc, p, 16); break;
        case GLB_EMISSION:            memcpy(G.mEmi, p, 16); break;
        case GLB_SHININESS:           G.shininess = p[0]; break;
        default: return;
    }
    G.uniformsDirty = 1;
}

static void do_light_model(uint32_t pname, const float* p) {
    if (!p) return;
    switch (pname) {
        case GLB_LIGHT_MODEL_AMBIENT:      memcpy(G.sceneAmb, p, 16); break;
        case GLB_LIGHT_MODEL_TWO_SIDE:     G.twoSided = (p[0] != 0.0f); break;
        case GLB_LIGHT_MODEL_LOCAL_VIEWER: G.localViewer = (p[0] != 0.0f); break;
        default: return;
    }
    G.uniformsDirty = 1;
}

void glb_light(uint32_t light, uint32_t pname, const float* params) {
    RECV(OP_LIGHT, light, pname, params, 4);
    do_light(light, pname, params);
}
void glb_material(uint32_t face, uint32_t pname, const float* params) {
    RECV(OP_MATERIAL, face, pname, params, (pname == GLB_SHININESS) ? 1 : 4);
    do_material(face, pname, params);
}
void glb_light_model(uint32_t pname, const float* params) {
    RECV(OP_LIGHT_MODEL, pname, 0, params, (pname == GLB_LIGHT_MODEL_AMBIENT) ? 4 : 1);
    do_light_model(pname, params);
}
void glb_color_material(uint32_t face, uint32_t mode) {
    RECF(OP_COLOR_MATERIAL, face, mode, 0.0f);
    G.colorMatMode = mode;
}

// ============================================================================ textures =========
void glb_gen_textures(int n, uint32_t* ids) {
    static uint32_t next = 1;
    for (int i = 0; i < n; i++) ids[i] = next++;
}

void glb_delete_textures(int n, const uint32_t* ids) {
    for (int i = 0; i < n; i++) {
        [G.textures removeObjectForKey:@(ids[i])];
        if (G.boundTexId == ids[i]) G.boundTexId = 0;
    }
}

void glb_bind_texture(uint32_t target, uint32_t id) {
    RECF(OP_BIND_TEXTURE, target, id, 0.0f);
    (void)target;
    G.boundTexId = id;
}

void glb_tex_image_2d(uint32_t target, int level, int internalformat,
                      int w, int h, int border,
                      uint32_t format, uint32_t type, const void* pixels) {
    (void)target; (void)internalformat; (void)border; (void)type;
    if (!G.inited || level != 0 || w <= 0 || h <= 0 || G.boundTexId == 0) return;
    @autoreleasepool {
        MTLTextureDescriptor* d =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                               width:(NSUInteger)w
                                                              height:(NSUInteger)h
                                                           mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead;
        d.storageMode = MTLStorageModeShared;
        id<MTLTexture> t = [G.dev newTextureWithDescriptor:d];
        if (!t) return;
        if (pixels) {
            if (format == GLB_RGBA) {
                [t replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h)
                     mipmapLevel:0 withBytes:pixels bytesPerRow:(NSUInteger)w * 4];
            } else {
                int src_bpp = (format == GLB_RGB) ? 3 : 1;
                uint8_t* tmp = (uint8_t*)malloc((size_t)w * h * 4);
                if (tmp) {
                    const uint8_t* s = (const uint8_t*)pixels;
                    for (size_t i = 0, np = (size_t)w * h; i < np; i++) {
                        if (src_bpp == 3) {
                            tmp[i * 4 + 0] = s[i * 3 + 0];
                            tmp[i * 4 + 1] = s[i * 3 + 1];
                            tmp[i * 4 + 2] = s[i * 3 + 2];
                        } else {
                            tmp[i * 4 + 0] = tmp[i * 4 + 1] = tmp[i * 4 + 2] = s[i];
                        }
                        tmp[i * 4 + 3] = 255;
                    }
                    [t replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h)
                         mipmapLevel:0 withBytes:tmp bytesPerRow:(NSUInteger)w * 4];
                    free(tmp);
                }
            }
        }
        G.textures[@(G.boundTexId)] = t;
    }
}

void glb_tex_parameter_i(uint32_t target, uint32_t pname, int param) {
    (void)target;
    switch (pname) {
        case GLB_TEXTURE_MIN_FILTER: G.texMin = (uint32_t)param; break;
        case GLB_TEXTURE_MAG_FILTER: G.texMag = (uint32_t)param; break;
        case GLB_TEXTURE_WRAP_S:     G.texWrapS = (uint32_t)param; break;
        case GLB_TEXTURE_WRAP_T:     G.texWrapT = (uint32_t)param; break;
        default: break;
    }
}

// =============================================================================== frame =========
static void do_clear(uint32_t mask, float r, float g, float b, float a, float depth) {
    begin_frame_if_needed();
    end_encoder();                       // a clear starts a brand-new pass
    G.pendingClearMask = mask;
    G.pendingClear[0] = r; G.pendingClear[1] = g; G.pendingClear[2] = b; G.pendingClear[3] = a;
    G.pendingClearDepth = depth;
    open_encoder();                      // ... which is opened right away, so the clear happens
}

void glb_clear(uint32_t mask, float r, float g, float b, float a, float depth) {
    RECF(OP_CLEAR, mask, 0, r, g, b, a, depth);
    do_clear(mask, r, g, b, a, depth);
}

void glb_clear_color(float r, float g, float b, float a) {
    RECF(OP_CLEAR_COLOR, 0, 0, r, g, b, a);
    G.clearColor[0] = r; G.clearColor[1] = g; G.clearColor[2] = b; G.clearColor[3] = a;
}

void glb_clear_depth_value(float d) { RECF(OP_CLEAR_DEPTH, 0, 0, d); G.clearDepth = d; }

void glb_clear_current(uint32_t mask) {
    RECF(OP_CLEAR, mask, 0,
         G.clearColor[0], G.clearColor[1], G.clearColor[2], G.clearColor[3], G.clearDepth);
    do_clear(mask, G.clearColor[0], G.clearColor[1], G.clearColor[2], G.clearColor[3], G.clearDepth);
}

uint64_t glb_swap(void) {
    if (!G.inited) return 0;
    @autoreleasepool {
        begin_frame_if_needed();
        uint64_t n = G.frameCounter;
        commit_frame();
        return n;
    }
}

void glb_frame_stats(uint64_t* draws, uint64_t* vertices) {
    if (draws) *draws = G.drawsLast;
    if (vertices) *vertices = G.vertsLast;
}

// ============================================================================== replay =========
static void replay(const GlbList* l) {
    uint32_t saveRec = G.recording;
    int saveExec = G.recExec;
    G.recording = 0;   // replayed commands execute; they are not re-recorded
    G.recExec = 0;

    for (uint32_t i = 0; i < l->count; i++) {
        const GlbCmd* c = &l->cmds[i];
        const float* f = c->f;
        switch (c->op) {
            case OP_MATRIX_MODE:   do_matrix_mode(c->a); break;
            case OP_LOAD_IDENTITY: mat_identity(cur_stack_top()); G.uniformsDirty = 1; break;
            case OP_LOAD_MATRIX:   memcpy(cur_stack_top(), f, 64); G.uniformsDirty = 1; break;
            case OP_MULT_MATRIX:   mat_post(f); break;
            case OP_PUSH:          do_push(); break;
            case OP_POP:           do_pop(); break;
            case OP_TRANSLATE:     do_translate(f[0], f[1], f[2]); break;
            case OP_ROTATE:        do_rotate(f[0], f[1], f[2], f[3]); break;
            case OP_SCALE:         do_scale(f[0], f[1], f[2]); break;
            case OP_FRUSTUM:       do_frustum(f[0], f[1], f[2], f[3], f[4], f[5]); break;
            case OP_ORTHO:         do_ortho(f[0], f[1], f[2], f[3], f[4], f[5]); break;
            case OP_BEGIN:         do_begin(c->a); break;
            case OP_VERTEX:        im_push(f[0], f[1], f[2]); break;
            case OP_NORMAL:        G.curNormal[0] = f[0]; G.curNormal[1] = f[1]; G.curNormal[2] = f[2]; break;
            case OP_COLOR:         memcpy(G.curColor, f, 16); break;
            case OP_TEXCOORD:      G.curUV[0] = f[0]; G.curUV[1] = f[1]; break;
            case OP_END:           do_end(); break;
            case OP_ENABLE:        do_enable(c->a, 1); break;
            case OP_DISABLE:       do_enable(c->a, 0); break;
            case OP_DEPTH_FUNC:    G.depthFunc = c->a; break;
            case OP_DEPTH_MASK:    G.depthWrite = (int)c->a; break;
            case OP_CULL_FACE:     G.cullFace = c->a; break;
            case OP_FRONT_FACE:    G.frontFace = c->a; break;
            case OP_SHADE_MODEL:   G.shadeModel = c->a; break;
            case OP_BLEND_FUNC:    G.blendSrc = c->a; G.blendDst = c->b; break;
            case OP_POINT_SIZE:    G.pointSize = f[0]; G.uniformsDirty = 1; break;
            case OP_LIGHT:         do_light(c->a, c->b, f); break;
            case OP_MATERIAL:      do_material(c->a, c->b, f); break;
            case OP_LIGHT_MODEL:   do_light_model(c->a, f); break;
            case OP_COLOR_MATERIAL:G.colorMatMode = c->b; break;
            case OP_BIND_TEXTURE:  G.boundTexId = c->b; break;
            case OP_VIEWPORT:      G.vpX = (int)f[0]; G.vpY = (int)f[1];
                                   G.vpW = (int)f[2]; G.vpH = (int)f[3];
                                   G.viewportDirty = 1; break;
            case OP_CLEAR:         do_clear(c->a, f[0], f[1], f[2], f[3], f[4]); break;
            case OP_CLEAR_COLOR:   memcpy(G.clearColor, f, 16); break;
            case OP_CLEAR_DEPTH:   G.clearDepth = f[0]; break;
            case OP_CALL_LIST: {
                GlbList* sub = list_get(c->a);
                if (sub && G.replayDepth < GLB_MAX_LIST_DEPTH) {
                    G.replayDepth++;
                    replay(sub);
                    G.replayDepth--;
                }
                break;
            }
            default: break;
        }
    }

    G.recording = saveRec;
    G.recExec = saveExec;
}
