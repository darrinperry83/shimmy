// test_glbridge.m — proves glbridge actually renders.
//
//   clang -arch arm64 -O2 -ObjC -fobjc-arc -o test_glbridge test_glbridge.m glbridge.m \
//         -framework Metal -framework Foundation -framework CoreGraphics -framework ImageIO
//
// What it does:
//   1. Builds the real glxgears geometry (toothed wheel from quad strips: front/back faces,
//      tooth faces, outward tooth walls, inner cylinder) into a DISPLAY LIST per gear.
//   2. Renders the 3-gear scene with GL_LIGHTING + GL_LIGHT0 + GL_DEPTH_TEST + GL_CULL_FACE
//      through a perspective frustum, and writes tests/gear.png.
//   3. Asserts: coverage, depth occlusion (order-independent), backface culling, and colour
//      variance across the gear (i.e. lighting is doing something).
//   4. Benchmarks N frames of the 3-gear scene.

#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "glbridge.h"

#define FB_W 800
#define FB_H 600
#define BENCH_FRAMES 600

static int g_failures = 0;

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (cond) { printf("  PASS  "); printf(__VA_ARGS__); printf("\n"); }     \
        else { printf("  FAIL  "); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
    } while (0)

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// ------------------------------------------------------------------------------- PNG out ------
static int write_png(const char* path, const uint8_t* rgba, int w, int h) {
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate((void*)rgba, (size_t)w, (size_t)h, 8,
                                             (size_t)w * 4, cs,
                                             kCGImageAlphaNoneSkipLast | kCGBitmapByteOrder32Big);
    if (!ctx) { CGColorSpaceRelease(cs); return -1; }
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CFStringRef p = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
    CFURLRef url = CFURLCreateWithFileSystemPath(NULL, p, kCFURLPOSIXPathStyle, false);
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
    int rc = -1;
    if (dst) {
        CGImageDestinationAddImage(dst, img, NULL);
        rc = CGImageDestinationFinalize(dst) ? 0 : -1;
        CFRelease(dst);
    }
    CFRelease(url); CFRelease(p);
    CGImageRelease(img);
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    return rc;
}

// ---------------------------------------------------------------------------- gear geometry ---
// The glxgears wheel: r0 = bore radius, r1 = root radius, r2 = tip radius, `width` deep along z.
static void gear(float inner_radius, float outer_radius, float width, int teeth, float tooth_depth) {
    const float r0 = inner_radius;
    const float r1 = outer_radius - tooth_depth * 0.5f;
    const float r2 = outer_radius + tooth_depth * 0.5f;
    const float da = 2.0f * (float)M_PI / (float)teeth / 4.0f;
    const float hw = width * 0.5f;
    const float step = 2.0f * (float)M_PI / (float)teeth;

    glb_shade_model(GLB_FLAT);

    // ---- front face ----
    glb_normal3f(0.0f, 0.0f, 1.0f);
    glb_begin(GLB_QUAD_STRIP);
    for (int i = 0; i <= teeth; i++) {
        float a = (float)i * step;
        glb_vertex3f(r0 * cosf(a), r0 * sinf(a), hw);
        glb_vertex3f(r1 * cosf(a), r1 * sinf(a), hw);
        if (i < teeth) {
            glb_vertex3f(r0 * cosf(a), r0 * sinf(a), hw);
            glb_vertex3f(r1 * cosf(a + 3 * da), r1 * sinf(a + 3 * da), hw);
        }
    }
    glb_end();

    // ---- front faces of the teeth ----
    glb_begin(GLB_QUADS);
    for (int i = 0; i < teeth; i++) {
        float a = (float)i * step;
        glb_vertex3f(r1 * cosf(a), r1 * sinf(a), hw);
        glb_vertex3f(r2 * cosf(a + da), r2 * sinf(a + da), hw);
        glb_vertex3f(r2 * cosf(a + 2 * da), r2 * sinf(a + 2 * da), hw);
        glb_vertex3f(r1 * cosf(a + 3 * da), r1 * sinf(a + 3 * da), hw);
    }
    glb_end();

    // ---- back face ----
    glb_normal3f(0.0f, 0.0f, -1.0f);
    glb_begin(GLB_QUAD_STRIP);
    for (int i = 0; i <= teeth; i++) {
        float a = (float)i * step;
        glb_vertex3f(r1 * cosf(a), r1 * sinf(a), -hw);
        glb_vertex3f(r0 * cosf(a), r0 * sinf(a), -hw);
        if (i < teeth) {
            glb_vertex3f(r1 * cosf(a + 3 * da), r1 * sinf(a + 3 * da), -hw);
            glb_vertex3f(r0 * cosf(a), r0 * sinf(a), -hw);
        }
    }
    glb_end();

    // ---- back faces of the teeth ----
    glb_begin(GLB_QUADS);
    for (int i = 0; i < teeth; i++) {
        float a = (float)i * step;
        glb_vertex3f(r1 * cosf(a + 3 * da), r1 * sinf(a + 3 * da), -hw);
        glb_vertex3f(r2 * cosf(a + 2 * da), r2 * sinf(a + 2 * da), -hw);
        glb_vertex3f(r2 * cosf(a + da), r2 * sinf(a + da), -hw);
        glb_vertex3f(r1 * cosf(a), r1 * sinf(a), -hw);
    }
    glb_end();

    // ---- outward faces of the teeth ----
    glb_begin(GLB_QUAD_STRIP);
    for (int i = 0; i < teeth; i++) {
        float a = (float)i * step;
        float u, v, len;

        glb_vertex3f(r1 * cosf(a), r1 * sinf(a), hw);
        glb_vertex3f(r1 * cosf(a), r1 * sinf(a), -hw);

        u = r2 * cosf(a + da) - r1 * cosf(a);
        v = r2 * sinf(a + da) - r1 * sinf(a);
        len = sqrtf(u * u + v * v);
        u /= len; v /= len;
        glb_normal3f(v, -u, 0.0f);
        glb_vertex3f(r2 * cosf(a + da), r2 * sinf(a + da), hw);
        glb_vertex3f(r2 * cosf(a + da), r2 * sinf(a + da), -hw);

        glb_normal3f(cosf(a), sinf(a), 0.0f);
        glb_vertex3f(r2 * cosf(a + 2 * da), r2 * sinf(a + 2 * da), hw);
        glb_vertex3f(r2 * cosf(a + 2 * da), r2 * sinf(a + 2 * da), -hw);

        u = r1 * cosf(a + 3 * da) - r2 * cosf(a + 2 * da);
        v = r1 * sinf(a + 3 * da) - r2 * sinf(a + 2 * da);
        len = sqrtf(u * u + v * v);
        if (len > 1e-6f) { u /= len; v /= len; }
        glb_normal3f(v, -u, 0.0f);
        glb_vertex3f(r1 * cosf(a + 3 * da), r1 * sinf(a + 3 * da), hw);
        glb_vertex3f(r1 * cosf(a + 3 * da), r1 * sinf(a + 3 * da), -hw);

        glb_normal3f(cosf(a), sinf(a), 0.0f);
    }
    glb_vertex3f(r1, 0.0f, hw);
    glb_vertex3f(r1, 0.0f, -hw);
    glb_end();

    // ---- inside radius cylinder (smooth-shaded) ----
    glb_shade_model(GLB_SMOOTH);
    glb_begin(GLB_QUAD_STRIP);
    for (int i = 0; i <= teeth; i++) {
        float a = (float)i * step;
        glb_normal3f(-cosf(a), -sinf(a), 0.0f);
        glb_vertex3f(r0 * cosf(a), r0 * sinf(a), -hw);
        glb_vertex3f(r0 * cosf(a), r0 * sinf(a), hw);
    }
    glb_end();
}

// --------------------------------------------------------------------------------- scene ------
static uint32_t g_gear1, g_gear2, g_gear3;

static void build_gear_lists(void) {
    static const float red[4]   = {0.8f, 0.1f, 0.0f, 1.0f};
    static const float green[4] = {0.0f, 0.8f, 0.2f, 1.0f};
    static const float blue[4]  = {0.2f, 0.2f, 1.0f, 1.0f};

    g_gear1 = glb_gen_lists(1);
    glb_new_list(g_gear1, GLB_COMPILE);
    glb_material(GLB_FRONT, GLB_AMBIENT_AND_DIFFUSE, red);
    gear(1.0f, 4.0f, 1.0f, 20, 0.7f);
    glb_end_list();

    g_gear2 = glb_gen_lists(1);
    glb_new_list(g_gear2, GLB_COMPILE);
    glb_material(GLB_FRONT, GLB_AMBIENT_AND_DIFFUSE, green);
    gear(0.5f, 2.0f, 2.0f, 10, 0.7f);
    glb_end_list();

    g_gear3 = glb_gen_lists(1);
    glb_new_list(g_gear3, GLB_COMPILE);
    glb_material(GLB_FRONT, GLB_AMBIENT_AND_DIFFUSE, blue);
    gear(1.3f, 2.0f, 0.5f, 10, 0.7f);
    glb_end_list();
}

static void scene_setup(int w, int h) {
    static const float light_pos[4] = {5.0f, 5.0f, 10.0f, 0.0f};

    glb_viewport(0, 0, w, h);
    glb_matrix_mode(GLB_PROJECTION);
    glb_load_identity();
    float aspect = (float)h / (float)w;
    glb_frustum(-1.0f, 1.0f, -aspect, aspect, 5.0f, 60.0f);

    glb_matrix_mode(GLB_MODELVIEW);
    glb_load_identity();
    // glLightfv(GL_POSITION) is transformed by the modelview in force *now* — identity here, so
    // the light stays fixed in eye space exactly as glxgears intends.
    glb_light(GLB_LIGHT0, GLB_POSITION, light_pos);
    glb_enable(GLB_CULL_FACE);
    glb_enable(GLB_LIGHTING);
    glb_enable(GLB_LIGHT0);
    glb_enable(GLB_DEPTH_TEST);
    glb_depth_func(GLB_LESS);
    glb_translate(0.0f, 0.0f, -40.0f);
}

// One frame of the classic 3-gear scene, drawn entirely through glb_call_list.
static void draw_scene(float angle) {
    const float view_rotx = 20.0f, view_roty = 30.0f, view_rotz = 0.0f;

    glb_clear(GLB_COLOR_BUFFER_BIT | GLB_DEPTH_BUFFER_BIT, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f);

    glb_push_matrix();
    glb_rotate(view_rotx, 1.0f, 0.0f, 0.0f);
    glb_rotate(view_roty, 0.0f, 1.0f, 0.0f);
    glb_rotate(view_rotz, 0.0f, 0.0f, 1.0f);

    glb_push_matrix();
    glb_translate(-3.0f, -2.0f, 0.0f);
    glb_rotate(angle, 0.0f, 0.0f, 1.0f);
    glb_call_list(g_gear1);
    glb_pop_matrix();

    glb_push_matrix();
    glb_translate(3.1f, -2.0f, 0.0f);
    glb_rotate(-2.0f * angle - 9.0f, 0.0f, 0.0f, 1.0f);
    glb_call_list(g_gear2);
    glb_pop_matrix();

    glb_push_matrix();
    glb_translate(-3.1f, 4.2f, 0.0f);
    glb_rotate(-2.0f * angle - 25.0f, 0.0f, 0.0f, 1.0f);
    glb_call_list(g_gear3);
    glb_pop_matrix();

    glb_pop_matrix();
}

// -------------------------------------------------------------------------- image analysis ----
typedef struct { double r, g, b; } Rgb;

static Rgb pixel_at(const uint8_t* p, int w, int x, int y) {
    const uint8_t* q = p + ((size_t)y * (size_t)w + (size_t)x) * 4;
    Rgb c = {q[0] / 255.0, q[1] / 255.0, q[2] / 255.0};
    return c;
}

// ============================================================================== unit tests =====
static void test_depth_occlusion(void) {
    // Ortho box; the "near" quad is at z=-1, the "far" quad at z=-5. With GL_LESS the near one
    // must win regardless of the order the two are submitted in.
    static uint8_t px[64 * 64 * 4];

    for (int order = 0; order < 2; order++) {
        glb_resize(64, 64);
        glb_viewport(0, 0, 64, 64);
        glb_matrix_mode(GLB_PROJECTION);
        glb_load_identity();
        glb_ortho(-1, 1, -1, 1, 0.1f, 10.0f);
        glb_matrix_mode(GLB_MODELVIEW);
        glb_load_identity();
        glb_disable(GLB_LIGHTING);
        glb_disable(GLB_CULL_FACE);
        glb_enable(GLB_DEPTH_TEST);
        glb_depth_func(GLB_LESS);

        glb_clear(GLB_COLOR_BUFFER_BIT | GLB_DEPTH_BUFFER_BIT, 0, 0, 0, 1, 1.0f);

        float zs[2]      = {-1.0f, -5.0f};      // [0] near (green), [1] far (red)
        float cols[2][3] = {{0, 1, 0}, {1, 0, 0}};
        for (int k = 0; k < 2; k++) {
            int i = order ? (1 - k) : k;        // order 0: near first; order 1: far first
            glb_color4f(cols[i][0], cols[i][1], cols[i][2], 1.0f);
            glb_begin(GLB_QUADS);
            glb_vertex3f(-0.8f, -0.8f, zs[i]);
            glb_vertex3f( 0.8f, -0.8f, zs[i]);
            glb_vertex3f( 0.8f,  0.8f, zs[i]);
            glb_vertex3f(-0.8f,  0.8f, zs[i]);
            glb_end();
        }
        glb_swap();
        glb_readback(px, 64, 64);

        Rgb c = pixel_at(px, 64, 32, 32);
        CHECK(c.g > 0.9 && c.r < 0.1,
              "depth occlusion (%s first): centre is green (%.2f,%.2f,%.2f)",
              order ? "far" : "near", c.r, c.g, c.b);
    }
}

static void test_backface_cull(void) {
    // Front-facing (GL_CCW) triangle must survive GL_BACK culling; the same triangle wound the
    // other way must vanish. This is what pins the GL->Metal winding mapping.
    static uint8_t px[64 * 64 * 4];
    int visible[2] = {0, 0};

    for (int wind = 0; wind < 2; wind++) {
        glb_resize(64, 64);
        glb_viewport(0, 0, 64, 64);
        glb_matrix_mode(GLB_PROJECTION);
        glb_load_identity();
        glb_ortho(-1, 1, -1, 1, 0.1f, 10.0f);
        glb_matrix_mode(GLB_MODELVIEW);
        glb_load_identity();
        glb_disable(GLB_LIGHTING);
        glb_disable(GLB_DEPTH_TEST);
        glb_enable(GLB_CULL_FACE);
        glb_cull_face(GLB_BACK);
        glb_front_face(GLB_CCW);

        glb_clear(GLB_COLOR_BUFFER_BIT | GLB_DEPTH_BUFFER_BIT, 0, 0, 0, 1, 1.0f);
        glb_color4f(1, 1, 1, 1);
        glb_begin(GLB_TRIANGLES);
        if (wind == 0) {                        // CCW in GL's y-up NDC => front facing
            glb_vertex3f(-0.8f, -0.8f, -1.0f);
            glb_vertex3f( 0.8f, -0.8f, -1.0f);
            glb_vertex3f( 0.0f,  0.8f, -1.0f);
        } else {                                // CW => back facing, must be culled
            glb_vertex3f( 0.0f,  0.8f, -1.0f);
            glb_vertex3f( 0.8f, -0.8f, -1.0f);
            glb_vertex3f(-0.8f, -0.8f, -1.0f);
        }
        glb_end();
        glb_swap();
        glb_readback(px, 64, 64);
        visible[wind] = pixel_at(px, 64, 32, 40).r > 0.5;   // inside the triangle
    }

    CHECK(visible[0] == 1, "cull: CCW (front-facing) triangle is drawn");
    CHECK(visible[1] == 0, "cull: CW (back-facing) triangle is culled");
}

static void test_orientation(void) {
    // A quad occupying only the TOP half of GL's y-up clip space must land in the top rows of the
    // readback buffer (row 0 first) — otherwise every frame reaches the window upside down.
    static uint8_t px[64 * 64 * 4];
    glb_resize(64, 64);
    glb_viewport(0, 0, 64, 64);
    glb_matrix_mode(GLB_PROJECTION);
    glb_load_identity();
    glb_ortho(-1, 1, -1, 1, 0.1f, 10.0f);
    glb_matrix_mode(GLB_MODELVIEW);
    glb_load_identity();
    glb_disable(GLB_LIGHTING);
    glb_disable(GLB_CULL_FACE);
    glb_disable(GLB_DEPTH_TEST);
    glb_clear(GLB_COLOR_BUFFER_BIT | GLB_DEPTH_BUFFER_BIT, 0, 0, 0, 1, 1.0f);
    glb_color4f(1, 1, 1, 1);
    glb_begin(GLB_QUADS);
    glb_vertex3f(-0.9f, 0.2f, -1.0f);
    glb_vertex3f( 0.9f, 0.2f, -1.0f);
    glb_vertex3f( 0.9f, 0.9f, -1.0f);
    glb_vertex3f(-0.9f, 0.9f, -1.0f);
    glb_end();
    glb_swap();
    glb_readback(px, 64, 64);

    Rgb top = pixel_at(px, 64, 32, 8);
    Rgb bot = pixel_at(px, 64, 32, 56);
    CHECK(top.r > 0.5 && bot.r < 0.5,
          "orientation: GL +y lands in the top rows (top=%.2f bottom=%.2f)", top.r, bot.r);
}

static void test_blend(void) {
    static uint8_t px[64 * 64 * 4];
    glb_resize(64, 64);
    glb_viewport(0, 0, 64, 64);
    glb_matrix_mode(GLB_PROJECTION);
    glb_load_identity();
    glb_ortho(-1, 1, -1, 1, 0.1f, 10.0f);
    glb_matrix_mode(GLB_MODELVIEW);
    glb_load_identity();
    glb_disable(GLB_LIGHTING);
    glb_disable(GLB_CULL_FACE);
    glb_disable(GLB_DEPTH_TEST);
    glb_clear(GLB_COLOR_BUFFER_BIT | GLB_DEPTH_BUFFER_BIT, 0, 0, 0, 1, 1.0f);

    glb_color4f(1, 0, 0, 1);                    // opaque red base
    glb_begin(GLB_QUADS);
    glb_vertex3f(-0.9f, -0.9f, -1.0f); glb_vertex3f(0.9f, -0.9f, -1.0f);
    glb_vertex3f( 0.9f,  0.9f, -1.0f); glb_vertex3f(-0.9f, 0.9f, -1.0f);
    glb_end();

    glb_enable(GLB_BLEND);
    glb_blend_func(GLB_SRC_ALPHA, GLB_ONE_MINUS_SRC_ALPHA);
    glb_color4f(0, 0, 1, 0.5f);                 // half-transparent blue on top
    glb_begin(GLB_QUADS);
    glb_vertex3f(-0.9f, -0.9f, -1.0f); glb_vertex3f(0.9f, -0.9f, -1.0f);
    glb_vertex3f( 0.9f,  0.9f, -1.0f); glb_vertex3f(-0.9f, 0.9f, -1.0f);
    glb_end();
    glb_disable(GLB_BLEND);
    glb_swap();
    glb_readback(px, 64, 64);

    Rgb c = pixel_at(px, 64, 32, 32);
    CHECK(fabs(c.r - 0.5) < 0.05 && fabs(c.b - 0.5) < 0.05,
          "blend: SRC_ALPHA/ONE_MINUS_SRC_ALPHA gives 50%% purple (%.2f,%.2f,%.2f)",
          c.r, c.g, c.b);
}

static void test_matrix_stack(void) {
    float m[16];
    glb_matrix_mode(GLB_MODELVIEW);
    glb_load_identity();
    glb_translate(1, 2, 3);
    glb_push_matrix();
    glb_translate(10, 20, 30);
    glb_get_matrix(GLB_MODELVIEW, m);
    int inner = (fabsf(m[12] - 11) < 1e-4f && fabsf(m[13] - 22) < 1e-4f && fabsf(m[14] - 33) < 1e-4f);
    glb_pop_matrix();
    glb_get_matrix(GLB_MODELVIEW, m);
    int outer = (fabsf(m[12] - 1) < 1e-4f && fabsf(m[13] - 2) < 1e-4f && fabsf(m[14] - 3) < 1e-4f);
    CHECK(inner && outer, "matrix stack: push/translate/pop restores the outer matrix");

    // glRotatef(90, 0,0,1) must take +x to +y.
    glb_load_identity();
    glb_rotate(90.0f, 0, 0, 1);
    glb_get_matrix(GLB_MODELVIEW, m);
    float x = m[0] * 1 + m[4] * 0, y = m[1] * 1 + m[5] * 0;
    CHECK(fabsf(x) < 1e-4f && fabsf(y - 1.0f) < 1e-4f,
          "matrix: rotate(90,z) maps +x to +y (got %.3f,%.3f)", x, y);
}

// ============================================================================== gear image =====
static void test_gear_image(void) {
    static uint8_t px[FB_W * FB_H * 4];

    glb_resize(FB_W, FB_H);
    scene_setup(FB_W, FB_H);
    draw_scene(35.0f);
    glb_swap();

    uint64_t draws = 0, verts = 0;
    glb_frame_stats(&draws, &verts);
    printf("  frame: %llu draw calls, %llu vertices\n",
           (unsigned long long)draws, (unsigned long long)verts);
    CHECK(draws >= 18, "display lists replayed: %llu draws in the frame (3 gears x 6 batches)",
          (unsigned long long)draws);

    if (glb_readback(px, FB_W, FB_H) != 0) { printf("  FAIL  readback\n"); g_failures++; return; }

    // ---- coverage ----
    size_t total = (size_t)FB_W * FB_H, lit = 0;
    double sum = 0, sum2 = 0;
    double rsum = 0, gsum = 0, bsum = 0;
    for (size_t i = 0; i < total; i++) {
        int r = px[i * 4], g = px[i * 4 + 1], b = px[i * 4 + 2];
        if (r > 8 || g > 8 || b > 8) {
            lit++;
            double lum = (0.299 * r + 0.587 * g + 0.114 * b) / 255.0;
            sum += lum; sum2 += lum * lum;
            rsum += r; gsum += g; bsum += b;
        }
    }
    double frac = (double)lit / (double)total;
    printf("  coverage: %.1f%% of pixels non-background\n", frac * 100.0);
    CHECK(frac > 0.08 && frac < 0.80, "a meaningful fraction of pixels is non-background (%.1f%%)",
          frac * 100.0);

    // ---- lighting produces shading variation ----
    double mean = sum / (double)lit;
    double var = sum2 / (double)lit - mean * mean;
    double sd = sqrt(var > 0 ? var : 0);
    printf("  luminance over drawn pixels: mean %.3f, stddev %.3f\n", mean, sd);
    CHECK(sd > 0.04, "colours vary across the gears -> lighting is shading them (sd=%.3f)", sd);

    // ---- all three gear colours present ----
    int nred = 0, ngreen = 0, nblue = 0;
    for (size_t i = 0; i < total; i++) {
        int r = px[i * 4], g = px[i * 4 + 1], b = px[i * 4 + 2];
        if (r < 12 && g < 12 && b < 12) continue;
        if (r > g + 20 && r > b + 20) nred++;
        else if (g > r + 20 && g > b + 10) ngreen++;
        else if (b > r + 20 && b > g + 20) nblue++;
    }
    printf("  gear pixels: red %d, green %d, blue %d (avg rgb %.0f/%.0f/%.0f)\n",
           nred, ngreen, nblue, rsum / lit, gsum / lit, bsum / lit);
    CHECK(nred > 2000 && ngreen > 1000 && nblue > 1000,
          "all three coloured gears are on screen");

    // ---- the image is not a flat wash: check the four corners are background ----
    Rgb c0 = pixel_at(px, FB_W, 4, 4);
    Rgb c1 = pixel_at(px, FB_W, FB_W - 5, FB_H - 5);
    CHECK(c0.r + c0.g + c0.b < 0.1 && c1.r + c1.g + c1.b < 0.1,
          "corners are background (scene does not fill the frame)");

    if (write_png("/Users/dperry/linux-shim/tests/gear.png", px, FB_W, FB_H) == 0)
        printf("  wrote /Users/dperry/linux-shim/tests/gear.png\n");
    else { printf("  FAIL  could not write gear.png\n"); g_failures++; }
}

// =============================================================================== benchmark =====
static void benchmark(void) {
    glb_resize(FB_W, FB_H);
    scene_setup(FB_W, FB_H);

    // warm up (shader/pipeline compilation happens on the first frames)
    for (int i = 0; i < 10; i++) { draw_scene((float)i); glb_swap(); }

    double t0 = now_sec();
    for (int i = 0; i < BENCH_FRAMES; i++) {
        draw_scene((float)i * 0.7f);
        glb_swap();
    }
    // Drain so the timing covers work actually finished on the GPU, not just enqueued.
    static uint8_t one[4];
    glb_readback(one, 1, 1);
    double dt = now_sec() - t0;

    uint64_t draws = 0, verts = 0;
    glb_frame_stats(&draws, &verts);
    printf("\n  %d frames of the 3-gear scene at %dx%d\n", BENCH_FRAMES, FB_W, FB_H);
    printf("  %llu draw calls / %llu vertices per frame\n",
           (unsigned long long)draws, (unsigned long long)verts);
    printf("  %.3f s total -> %.2f ms/frame, %.1f FPS\n",
           dt, dt * 1000.0 / BENCH_FRAMES, BENCH_FRAMES / dt);
}

// ==================================================================================== main =====
int main(void) {
    @autoreleasepool {
        int rc = glb_init(NULL);
        if (rc != 0) { fprintf(stderr, "glb_init failed: %d\n", rc); return 1; }

        [[NSFileManager defaultManager] createDirectoryAtPath:@"/Users/dperry/linux-shim/tests"
                                 withIntermediateDirectories:YES attributes:nil error:nil];

        printf("== matrix stack ==\n");        test_matrix_stack();
        printf("== orientation ==\n");         test_orientation();
        printf("== depth test ==\n");          test_depth_occlusion();
        printf("== backface culling ==\n");    test_backface_cull();
        printf("== blending ==\n");            test_blend();

        printf("== gear scene (display lists + lighting) ==\n");
        build_gear_lists();
        test_gear_image();

        printf("== benchmark ==\n");
        benchmark();

        glb_destroy();

        printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL TESTS PASSED",
               g_failures, g_failures == 1 ? "" : "s");
        return g_failures ? 1 : 0;
    }
}
