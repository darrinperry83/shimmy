// One GBM/EGL/GLES context. eglinfo tests many platform/API/version combinations
// and is not a focused check of whether the DRM render node can initialize EGL.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
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
  // eglChooseConfig() does not guarantee a matching native visual. A GBM
  // ARGB8888 surface paired with an RG16 EGLConfig returns EGL_BAD_MATCH.
  const EGLint config_attrs[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                                  EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
  EGLConfig config = NULL;
  EGLint count = 0;
  EGLConfig configs[128];
  if (!eglChooseConfig(display, config_attrs, configs, 128, &count) || count <= 0) {
    fprintf(stderr, "eglChooseConfig count=%d error=%#x\n", count, eglGetError()); return 5;
  }
  for (int i = 0; i < count && i < 128; i++) {
    EGLint visual = 0;
    if (eglGetConfigAttrib(display, configs[i], EGL_NATIVE_VISUAL_ID, &visual) &&
        visual == GBM_FORMAT_ARGB8888) { config = configs[i]; break; }
  }
  if (!config) { fprintf(stderr, "no ARGB8888 EGLConfig\n"); return 5; }
  if (!eglBindAPI(EGL_OPENGL_ES_API)) { fprintf(stderr, "eglBindAPI %#x\n", eglGetError()); return 6; }
  const EGLint context_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
  EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attrs);
  if (context == EGL_NO_CONTEXT) { fprintf(stderr, "eglCreateContext %#x\n", eglGetError()); return 7; }
  struct gbm_surface *surface = gbm_surface_create(gbm, 64, 64, GBM_FORMAT_ARGB8888,
                                                   GBM_BO_USE_RENDERING);
  if (!surface) { fprintf(stderr, "gbm_surface_create failed\n"); return 8; }
  EGLSurface window = eglCreateWindowSurface(display, config, (EGLNativeWindowType)surface, NULL);
  if (window == EGL_NO_SURFACE) { fprintf(stderr, "eglCreateWindowSurface %#x\n", eglGetError()); return 9; }
  if (!eglMakeCurrent(display, window, window, context)) {
    fprintf(stderr, "eglMakeCurrent %#x\n", eglGetError()); return 10;
  }
  const char *renderer = (const char *)glGetString(GL_RENDERER);
  if (!renderer) { fprintf(stderr, "glGetString(GL_RENDERER) failed\n"); return 11; }
  printf("GL_RENDERER=%s\n", renderer);
  glClearColor(1, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  glFlush();
  if (glGetError() != GL_NO_ERROR) { fprintf(stderr, "GLES clear failed\n"); return 12; }
  puts("GBM GLES2 window clear completed");
  eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroySurface(display, window);
  gbm_surface_destroy(surface);
  eglDestroyContext(display, context);
  eglTerminate(display);
  gbm_device_destroy(gbm);
  close(fd);
  return 0;
}
