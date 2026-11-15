/*
 * rdva_probe.c — headless decode probe for the rdmetal VAAPI -> VideoToolbox bridge.
 *
 * Mirrors vainfo's enumeration, then drives a real H.264 decode through libva: it feeds an
 * Annex-B bitstream (SPS+PPS+IDR) as a VASliceDataBufferType buffer, vaSyncSurface()s, and
 * derives the decoded NV12 image. A non-empty image proves the guest libva -> syscall 0x2003
 * -> VTDecompressionSession -> NV12-copy-back round-trip works.
 *
 * usage: rdva_probe [h264_file [width height]]
 *
 * Cross-compiled for aarch64 against the Arch rootfs (see vaapi/build.sh).
 */

#include <va/va.h>
#include <va/va_drm.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint64_t checksum(const unsigned char *p, size_t n) {
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
  return h;
}

static int count_nonzero(const unsigned char *p, size_t n) {
  int c = 0;
  for (size_t i = 0; i < n; i++) if (p[i]) c++;
  return c;
}

int main(int argc, char **argv) {
  const char *path = (argc > 1) ? argv[1] : "/root/test.h264";
  int width  = (argc > 2) ? atoi(argv[2]) : 320;
  int height = (argc > 3) ? atoi(argv[3]) : 240;

  int fd = open("/dev/dri/renderD128", O_RDWR);
  if (fd < 0) fd = open("/dev/dri/card0", O_RDWR);
  if (fd < 0) { fprintf(stderr, "rdva_probe: cannot open a DRM node\n"); return 1; }

  VADisplay dpy = vaGetDisplayDRM(fd);
  if (!dpy) { fprintf(stderr, "rdva_probe: vaGetDisplayDRM returned NULL\n"); return 2; }

  int major = 0, minor = 0;
  VAStatus st = vaInitialize(dpy, &major, &minor);
  if (st != VA_STATUS_SUCCESS) {
    fprintf(stderr, "rdva_probe: vaInitialize failed 0x%x\n", (unsigned)st);
    return 3;
  }
  printf("vaInitialize: %d.%d, vendor: %s\n", major, minor, vaQueryVendorString(dpy));

  /* enumeration (vainfo-style) */
  int nprof = vaMaxNumProfiles(dpy);
  VAProfile *profiles = calloc(nprof ? nprof : 1, sizeof *profiles);
  st = vaQueryConfigProfiles(dpy, profiles, &nprof);
  if (st != VA_STATUS_SUCCESS) { fprintf(stderr, "vaQueryConfigProfiles 0x%x\n", (unsigned)st); return 4; }
  for (int i = 0; i < nprof; i++) {
    int ne = vaMaxNumEntrypoints(dpy);
    VAEntrypoint *eps = calloc(ne ? ne : 1, sizeof *eps);
    st = vaQueryConfigEntrypoints(dpy, profiles[i], eps, &ne);
    if (st != VA_STATUS_SUCCESS) { free(eps); continue; }
    for (int j = 0; j < ne; j++)
      printf("  profile %d entrypoint %d\n", (int)profiles[i], (int)eps[j]);
    free(eps);
  }
  free(profiles);

  /* ---- decode one frame ---- */
  FILE *f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "rdva_probe: cannot open %s\n", path); return 5; }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char *h264 = malloc(sz ? sz : 1);
  if (fread(h264, 1, sz, f) != (size_t)sz) { fprintf(stderr, "short read\n"); return 6; }
  fclose(f);
  printf("bitstream: %s (%ld bytes)\n", path, sz);

  VAConfigAttrib attrs[2] = {
    { .type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420 },
    { .type = VAConfigAttribDecSliceMode, .value = VA_DEC_SLICE_MODE_NORMAL },
  };
  VAConfigID config = 0;
  st = vaCreateConfig(dpy, VAProfileH264Main, VAEntrypointVLD, attrs, 2, &config);
  if (st != VA_STATUS_SUCCESS) { fprintf(stderr, "vaCreateConfig 0x%x\n", (unsigned)st); return 7; }

  VASurfaceID surface = 0;
  st = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, width, height, &surface, 1, NULL, 0);
  if (st != VA_STATUS_SUCCESS) { fprintf(stderr, "vaCreateSurfaces 0x%x\n", (unsigned)st); return 8; }

  VAContextID context = 0;
  st = vaCreateContext(dpy, config, width, height, 0, &surface, 1, &context);
  if (st != VA_STATUS_SUCCESS) { fprintf(stderr, "vaCreateContext 0x%x\n", (unsigned)st); return 9; }

  VABufferID buf = 0;
  st = vaCreateBuffer(dpy, context, VASliceDataBufferType, (unsigned)sz, 1, h264, &buf);
  if (st != VA_STATUS_SUCCESS) { fprintf(stderr, "vaCreateBuffer 0x%x\n", (unsigned)st); return 10; }

  st = vaBeginPicture(dpy, context, surface);
  if (st != VA_STATUS_SUCCESS) { fprintf(stderr, "vaBeginPicture 0x%x\n", (unsigned)st); return 11; }
  st = vaRenderPicture(dpy, context, &buf, 1);
  if (st != VA_STATUS_SUCCESS) { fprintf(stderr, "vaRenderPicture 0x%x\n", (unsigned)st); return 12; }
  st = vaEndPicture(dpy, context);
  if (st != VA_STATUS_SUCCESS) { fprintf(stderr, "vaEndPicture 0x%x\n", (unsigned)st); return 13; }
  st = vaSyncSurface(dpy, surface);
  if (st != VA_STATUS_SUCCESS) {
    fprintf(stderr, "vaSyncSurface 0x%x (decode bridge failed)\n", (unsigned)st);
    return 14;
  }

  VAImage image;
  st = vaDeriveImage(dpy, surface, &image);
  if (st != VA_STATUS_SUCCESS) { fprintf(stderr, "vaDeriveImage 0x%x\n", (unsigned)st); return 15; }

  void *p = NULL;
  st = vaMapBuffer(dpy, image.buf, &p);
  if (st != VA_STATUS_SUCCESS) { fprintf(stderr, "vaMapBuffer 0x%x\n", (unsigned)st); return 16; }

  unsigned char *decoded = (unsigned char *)p;
  printf("decoded image: %ux%u format=0x%x data_size=%u planes=%u pitches=%u/%u offsets=%u/%u\n",
         image.width, image.height, image.format.fourcc, image.data_size, image.num_planes,
         image.pitches[0], image.pitches[1], image.offsets[0], image.offsets[1]);
  printf("decoded bytes: nonzero=%d checksum=0x%016llx\n",
         count_nonzero(decoded, image.data_size),
         (unsigned long long)checksum(decoded, image.data_size));

  vaUnmapBuffer(dpy, image.buf);
  vaDestroyImage(dpy, image.image_id);
  vaDestroyBuffer(dpy, buf);
  vaDestroyContext(dpy, context);
  vaDestroySurfaces(dpy, &surface, 1);
  vaDestroyConfig(dpy, config);
  vaTerminate(dpy);

  if (image.data_size == 0 || count_nonzero(decoded, image.data_size) == 0) {
    fprintf(stderr, "FAIL: empty decoded buffer\n");
    return 20;
  }
  printf("PASS: decoded %u bytes through the VideoToolbox bridge\n", image.data_size);
  return 0;
}
