#ifndef RDMETAL_DRAW_WIRE_H
#define RDMETAL_DRAW_WIRE_H
#include <stdint.h>

/* Test protocol for one generic Gallium draw. No pointers, shader pins,
 * IOSurface IDs or host authority are present. Production transport must
 * authenticate the owner and resolve target_handle in its own table. */
#define RDMETAL_DRAW_MAGIC 0x52444452u /* RDDR */
#define RDMETAL_DRAW_VERSION 1u
#define RDMETAL_DRAW_VERSION_TEXTURE 2u
#define RDMETAL_DRAW_VERSION_V3 3u
#define RDMETAL_DRAW_MAX_SHADER_WORDS 16384u
#define RDMETAL_DRAW_MAX_VERTEX_BYTES 262144u
#define RDMETAL_DRAW_MAX_INDEX_BYTES 262144u
#define RDMETAL_DRAW_MAX_UNIFORM_BYTES 64u /* one mat4 */
#define RDMETAL_DRAW_ATTR_NONE 0u
#define RDMETAL_DRAW_ATTR_FLOAT2 1u
#define RDMETAL_DRAW_ATTR_FLOAT3 2u
#define RDMETAL_DRAW_INDEX_NONE 0u
#define RDMETAL_DRAW_INDEX_U16 1u
#define RDMETAL_DRAW_INDEX_U32 2u
struct rdmetal_draw_header {
   uint32_t magic, version, total_bytes, flags;
   uint64_t target_handle;
   uint32_t target_width, target_height;
   uint32_t vertex_start, vertex_count, vertex_stride;
   uint32_t attribute_offset, attribute_format;
   uint32_t vertex_shader_words, fragment_shader_words, vertex_bytes;
   /* Little-endian payload: vertex SPIR-V, fragment SPIR-V, vertex bytes. */
};
_Static_assert(sizeof(struct rdmetal_draw_header)==64,"draw header ABI");
/* Version 2 keeps the complete version 1 prefix. The texture was uploaded
 * through a separate bounded broker operation; no pixel data or host pointer
 * travels with a draw. The broker resolves handle+generation itself. */
struct rdmetal_draw_header_v2 {
   struct rdmetal_draw_header base;
   uint64_t texture_handle, texture_generation;
   uint32_t sampler_flags, reserved;
};
_Static_assert(sizeof(struct rdmetal_draw_header_v2)==88,"draw texture header ABI");
/* Version 3 keeps the complete version 2 prefix and adds a real second vertex
 * attribute (texcoord), an optional index buffer and one small (mat4) constant
 * buffer. The vertex stage is always a canonical float3 position + float2
 * texcoord interleave; the fragment stage still samples texture slot 0 at the
 * forwarded texcoord when textured. Payload order after the v3 header:
 * vertex SPIR-V, fragment SPIR-V, vertex bytes, index bytes, uniform bytes. */
struct rdmetal_draw_header_v3 {
   struct rdmetal_draw_header_v2 base;
   uint32_t attr1_format;   /* RDMETAL_DRAW_ATTR_* or ATTR_NONE */
   uint32_t attr1_offset;   /* byte offset of texcoord within a vertex */
   uint32_t index_type;     /* RDMETAL_DRAW_INDEX_* */
   uint32_t index_count;    /* number of indices (0 when non-indexed) */
   uint32_t uniform_bytes;  /* 0 or RDMETAL_DRAW_MAX_UNIFORM_BYTES */
   uint32_t reserved;
};
_Static_assert(sizeof(struct rdmetal_draw_header_v3)==112,"draw v3 header ABI");
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "rdmetal draw wire currently requires little-endian producer"
#endif
#endif
