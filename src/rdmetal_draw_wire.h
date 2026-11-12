#ifndef RDMETAL_DRAW_WIRE_H
#define RDMETAL_DRAW_WIRE_H
#include <stdint.h>
#define RDMETAL_DRAW_MAGIC 0x52444452u
#define RDMETAL_DRAW_VERSION 1u
#define RDMETAL_DRAW_MAX_SHADER_WORDS 16384u
#define RDMETAL_DRAW_MAX_VERTEX_BYTES 262144u
#define RDMETAL_DRAW_ATTR_FLOAT2 1u
#define RDMETAL_DRAW_ATTR_FLOAT3 2u
struct rdmetal_draw_header {
   uint32_t magic, version, total_bytes, flags;
   uint64_t target_handle;
   uint32_t target_width, target_height;
   uint32_t vertex_start, vertex_count, vertex_stride;
   uint32_t attribute_offset, attribute_format;
   uint32_t vertex_shader_words, fragment_shader_words, vertex_bytes;
};
_Static_assert(sizeof(struct rdmetal_draw_header)==64,"draw packet header size");
#endif
