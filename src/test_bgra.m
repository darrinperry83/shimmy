// test_bgra.m — does BGRA-end-to-end reach the screen as the SAME COLOURS as the RGBA path did?
//
// WHY THIS FILE EXISTS. The compositor now hands window.m a B,G,R,A framebuffer and window.m
// picks MTLPixelFormatBGRA8Unorm for the staging texture instead of RGBA8Unorm. Every headless
// gate in this repo (c1-gate, gl-gate, the wl unit tests) stops at the CPU framebuffer — they
// read the PPM dump, which is written before Metal ever sees the pixels. So NOTHING that runs
// without a display could tell you whether the change swapped red and blue on screen, and a
// red/blue swap is exactly the failure a human eye does not reliably catch on a blue-grey window.
// That is a blind instrument, which on this project is how wrong results get believed.
//
// So this renders BOTH arms through the SAME pipeline window.m uses (the shader source below is
// copied verbatim from window.m's kBlitMSL, including its opaque-alpha line) into an OFFSCREEN
// BGRA8Unorm target — no window, no display, no /tmp/rdx-gui.lock — and reads the pixels back.
//
//   RUNG 1  RGBA8Unorm staging fed R,G,B,A  vs  BGRA8Unorm staging fed B,G,R,A
//           of the SAME image must come out BYTE-IDENTICAL. That is the whole claim.
//   RUNG 2  NEGATIVE CONTROL: feed the BGRA arm R,G,B,A bytes (the mutation the change invites)
//           and it must DIFFER. If it does not, rung 1 cannot see a swap and proves nothing.
//   RUNG 3  NEGATIVE CONTROL: the readback must not be uniform, or "identical" is trivially true
//           of two blank images — which is how a gate on this project once passed 100% black.
//
// Run it through tools/bgra-gate.sh, which compiles it and states the verdict.
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 129                       // deliberately not a round number or a vector multiple
#define H 71

static NSString* const kBlitMSL =
  @"#include <metal_stdlib>\n"
   "using namespace metal;\n"
   "struct VOut { float4 pos [[position]]; float2 uv; };\n"
   "vertex VOut v_blit(uint vid [[vertex_id]]) {\n"
   "  float2 uv = float2(float((vid << 1) & 2u), float(vid & 2u));\n"
   "  VOut o;\n"
   "  o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
   "  o.uv  = uv;\n"
   "  return o;\n"
   "}\n"
   "fragment float4 f_blit(VOut in [[stage_in]],\n"
   "                       texture2d<float> tex [[texture(0)]],\n"
   "                       sampler smp [[sampler(0)]]) {\n"
   "  return float4(tex.sample(smp, in.uv).rgb, 1.0);\n"
   "}\n";

static id<MTLDevice>              gDev;
static id<MTLCommandQueue>        gQ;
static id<MTLRenderPipelineState> gPipe;
static id<MTLSamplerState>        gSmp;

// Render `src` (already in `fmt` byte order) through the blit pipeline into an offscreen
// BGRA8Unorm target and read the result back. This mirrors window.m's present_source() exactly:
// same shader, same sampler, same fullscreen triangle, same destination format as the drawable.
static int blit_readback(const uint8_t* src, MTLPixelFormat fmt, uint8_t* out) {
  MTLTextureDescriptor* sd =
    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:W height:H mipmapped:NO];
  sd.usage = MTLTextureUsageShaderRead;
  sd.storageMode = MTLStorageModeShared;
  id<MTLTexture> stage = [gDev newTextureWithDescriptor:sd];

  MTLTextureDescriptor* dd =
    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                       width:W height:H mipmapped:NO];
  dd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  dd.storageMode = MTLStorageModeShared;
  id<MTLTexture> dst = [gDev newTextureWithDescriptor:dd];
  if (!stage || !dst) { fprintf(stderr, "texture alloc failed\n"); return -1; }

  [stage replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:src bytesPerRow:W * 4];

  MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
  rp.colorAttachments[0].texture     = dst;
  rp.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
  rp.colorAttachments[0].storeAction = MTLStoreActionStore;

  id<MTLCommandBuffer> cb = [gQ commandBuffer];
  id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
  [enc setRenderPipelineState:gPipe];
  [enc setFragmentTexture:stage atIndex:0];
  [enc setFragmentSamplerState:gSmp atIndex:0];
  [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  [enc endEncoding];
  [cb commit];
  [cb waitUntilCompleted];                       // a test, not a hot path
  if (cb.error) { fprintf(stderr, "gpu error: %s\n", cb.error.description.UTF8String); return -1; }

  [dst getBytes:out bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
  return 0;
}

int main(void) {
  @autoreleasepool {
    gDev = MTLCreateSystemDefaultDevice();
    if (!gDev) { fprintf(stderr, "no Metal device\n"); return 2; }
    gQ = [gDev newCommandQueue];
    NSError* err = nil;
    id<MTLLibrary> lib = [gDev newLibraryWithSource:kBlitMSL options:nil error:&err];
    if (!lib) { fprintf(stderr, "shader: %s\n", err.description.UTF8String); return 2; }
    MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction   = [lib newFunctionWithName:@"v_blit"];
    pd.fragmentFunction = [lib newFunctionWithName:@"f_blit"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    gPipe = [gDev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!gPipe) { fprintf(stderr, "pipeline: %s\n", err.description.UTF8String); return 2; }
    MTLSamplerDescriptor* sdsc = [MTLSamplerDescriptor new];
    sdsc.minFilter = MTLSamplerMinMagFilterLinear;
    sdsc.magFilter = MTLSamplerMinMagFilterLinear;
    gSmp = [gDev newSamplerStateWithDescriptor:sdsc];

    // A pattern where R, G and B are all DIFFERENT everywhere — otherwise a channel swap is
    // invisible by construction and every rung below is vacuous.
    static uint8_t rgba[W * H * 4], bgra[W * H * 4];
    for (int i = 0; i < W * H; i++) {
      uint8_t r = (uint8_t)(i * 7 + 11), g = (uint8_t)(i * 29 + 3), b = (uint8_t)(i * 53 + 200);
      if (r == b) b ^= 0x40;                       // guarantee R != B at every pixel
      rgba[i * 4 + 0] = r; rgba[i * 4 + 1] = g; rgba[i * 4 + 2] = b; rgba[i * 4 + 3] = 0xFF;
      bgra[i * 4 + 0] = b; bgra[i * 4 + 1] = g; bgra[i * 4 + 2] = r; bgra[i * 4 + 3] = 0xFF;
    }

    static uint8_t outA[W * H * 4], outB[W * H * 4], outC[W * H * 4];
    if (blit_readback(rgba, MTLPixelFormatRGBA8Unorm, outA) < 0) return 2;   // the old path
    if (blit_readback(bgra, MTLPixelFormatBGRA8Unorm, outB) < 0) return 2;   // the new path
    if (blit_readback(rgba, MTLPixelFormatBGRA8Unorm, outC) < 0) return 2;   // the mutation

    int fail = 0;

    // RUNG 3 first: if the readback is uniform, "identical" means nothing.
    int distinct = 0;
    for (int i = 1; i < W * H && distinct < 2; i++)
      if (memcmp(outA + i * 4, outA, 4) != 0) distinct = 2;
    if (distinct < 2) {
      printf("FAIL: the readback is a single flat colour — two blank images compare equal, so\n"
             "      rung 1 would pass on a dead pipeline\n");
      fail = 1;
    } else {
      printf("ok  : readback is a real image, not a flat fill\n");
    }

    // RUNG 1: the claim.
    if (memcmp(outA, outB, sizeof outA) == 0) {
      printf("ok  : RGBA8Unorm+RGBA bytes and BGRA8Unorm+BGRA bytes render BYTE-IDENTICALLY\n");
    } else {
      int n = 0, first = -1;
      for (int i = 0; i < W * H * 4; i++)
        if (outA[i] != outB[i]) { n++; if (first < 0) first = i; }
      printf("FAIL: the two paths differ in %d/%d bytes (first at %d: %02x vs %02x)\n",
             n, W * H * 4, first, outA[first], outB[first]);
      fail = 1;
    }

    // RUNG 2: the negative control. Wrong byte order into the BGRA arm must NOT pass.
    if (memcmp(outA, outC, sizeof outA) != 0) {
      printf("ok  : feeding the BGRA arm RGBA bytes DIFFERS — the comparison can see a swap\n");
    } else {
      printf("FAIL: red/blue crossed and rung 1's comparison still said equal. It is blind.\n");
      fail = 1;
    }

    printf("%s\n", fail ? "FAIL" : "PASS — BGRA end-to-end reaches the render target as the same "
                                   "colours the RGBA path produced, and a swap would be caught.");
    return fail;
  }
}
