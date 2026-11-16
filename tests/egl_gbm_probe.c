// One GBM/EGL/GLES context. eglinfo tests many platform/API/version combinations
// and is not a focused check of whether the DRM render node can initialize EGL.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <gbm.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
  int fd = open("/dev/dri/renderD128", O_RDWR);
  if (fd < 0) { perror("render node"); return 1; }
  struct gbm_device *gbm = gbm_create_device(fd);
  if (!gbm) { fprintf(stderr, "gbm_create_device failed\n"); return 2; }
  EGLDisplay display = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL);
  if (display == EGL_NO_DISPLAY) { fprintf(stderr, "eglGetPlatformDisplay %#x\n", eglGetError()); return 3; }
  EGLint major = 0, minor = 0;
  if (!eglInitialize(display, &major, &minor)) {
    fprintf(stderr, "eglInitialize %#x\n", eglGetError()); return 4;
  }
  printf("EGL %d.%d initialized\n", major, minor);
  const EGLint config_attrs[] = {
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE,
  };
  EGLConfig config = NULL;
  EGLint count = 0;
  if (!eglChooseConfig(display, config_attrs, &config, 1, &count) || count != 1) {
    fprintf(stderr, "eglChooseConfig count=%d error=%#x\n", count, eglGetError()); return 5;
  }
  if (!eglBindAPI(EGL_OPENGL_ES_API)) { fprintf(stderr, "eglBindAPI %#x\n", eglGetError()); return 6; }
  const EGLint context_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
  EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attrs);
  if (context == EGL_NO_CONTEXT) { fprintf(stderr, "eglCreateContext %#x\n", eglGetError()); return 7; }
  puts("GBM GLES2 context created");
  eglDestroyContext(display, context);
  eglTerminate(display);
  gbm_device_destroy(gbm);
  close(fd);
  return 0;
}
