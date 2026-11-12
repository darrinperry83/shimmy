#ifndef RD_SECONDARY_REGION_LAYOUT_H
#define RD_SECONDARY_REGION_LAYOUT_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#define RD_SECONDARY_MAX_REGIONS 64
#define RD_SECONDARY_BACKGROUND_ID UINT64_C(0x2000000000000002)

// Only an unpublished first manifest may defer a frame.
static int rd_secondary_manifest_unpublished(const char* path) {
  struct stat st;
  return path && lstat(path, &st) != 0 && errno == ENOENT;
}

static int rd_secondary_host_frame_matches(const char* expected,
                                           long x, long y, long width, long height) {
  long ex = 0, ey = 0, ew = 0, eh = 0;
  char extra = 0;
  return expected && sscanf(expected, "%ld,%ld,%ld,%ld%c", &ex, &ey, &ew,
                            &eh, &extra) == 4 &&
         ex == x && ey == y && ew == width && eh == height &&
         width > 0 && height > 0;
}

struct rd_secondary_region {
  uint64_t id;
  unsigned x, y, w, h;
  int role; // -1 background, 0 window, 1 panel
};

// Strict parser for the host-only atomic region file. A full output background
// must be present before the opaque second-output window may be ordered out.
static int rd_parse_secondary_regions(const char* text, unsigned output_w,
                                      unsigned output_h,
                                      struct rd_secondary_region out[RD_SECONDARY_MAX_REGIONS]) {
  if (!text || !output_w || !output_h || output_w > 16384 || output_h > 16384) return -1;
  unsigned count = 0;
  int background = 0;
  const char* start = text;
  while (*start) {
    const char* end = strchr(start, '\n');
    size_t length = end ? (size_t)(end - start) : strlen(start);
    if (!length) { start = end ? end + 1 : start + length; continue; }
    if (length >= 128 || count >= RD_SECONDARY_MAX_REGIONS) return -1;
    char line[128], role[16] = {0}, tail = 0;
    memcpy(line, start, length); line[length] = 0;
    unsigned long long id = 0;
    unsigned x = 0, y = 0, w = 0, h = 0;
    int fields = sscanf(line, "%llx %u %u %u %u %15s %c", &id, &x, &y, &w, &h, role, &tail);
    if (fields < 5 || fields > 6 || !id || !w || !h || x >= output_w ||
        y >= output_h || w > output_w - x || h > output_h - y) return -1;
    int kind = 0;
    if (fields == 6) {
      if (!strcmp(role, "background")) kind = -1;
      else if (!strcmp(role, "panel") || !strcmp(role, "popup")) kind = 1;
      else return -1;
    }
    if (kind == -1) {
      if (background || id != RD_SECONDARY_BACKGROUND_ID || x || y ||
          w != output_w || h != output_h || count) return -1;
      background = 1;
    } else if (id == RD_SECONDARY_BACKGROUND_ID) return -1;
    for (unsigned i = 0; i < count; i++) if (out[i].id == (uint64_t)id) return -1;
    out[count++] = (struct rd_secondary_region){(uint64_t)id, x, y, w, h, kind};
    start = end ? end + 1 : start + length;
  }
  return background ? (int)count : -1;
}

static int rd_secondary_pointer(const struct rd_secondary_region* crop,
                                int local_x, int local_y, int* output_x, int* output_y) {
  if (!crop || !output_x || !output_y || local_x < 0 || local_y < 0 ||
      (unsigned)local_x >= crop->w || (unsigned)local_y >= crop->h) return -1;
  *output_x = (int)crop->x + local_x;
  *output_y = (int)crop->y + local_y;
  return 0;
}

#endif
