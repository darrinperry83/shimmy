#define EGL_EGLEXT_PROTOTYPES
/* Adapted from the earlier integrated-Mesa ordinary_triangle guest probe. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <link.h>
#include <string.h>

static int module(struct dl_phdr_info *info, size_t bytes, void *opaque)
{
   (void)bytes; (void)opaque;
   if (info->dlpi_name && strstr(info->dlpi_name, "swrast_dri")) {
      fprintf(stderr, "RDMETAL_DRI_BASE=%p\n", (void *)info->dlpi_addr);
   }
   return 0;
}

static GLuint shader(GLenum kind, const char *source)
{
   GLuint s = glCreateShader(kind);
   glShaderSource(s, 1, &source, 0);
   glCompileShader(s);
   GLint ok = 0;
   glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
   return ok ? s : 0;
}

int main(void)
{
   PFNEGLGETPLATFORMDISPLAYEXTPROC platform =
      (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
   if (!platform) return 2;
   EGLDisplay display = platform(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, 0);
   if (!display || !eglInitialize(display, 0, 0)) return 3;
   dl_iterate_phdr(module, 0);
   EGLint attrs[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8,
      EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
   EGLConfig config; EGLint count = 0;
   if (!eglChooseConfig(display, attrs, &config, 1, &count) || !count) return 4;
   EGLint context_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
   EGLint surface_attrs[] = { EGL_WIDTH, 96, EGL_HEIGHT, 80, EGL_NONE };
   eglBindAPI(EGL_OPENGL_ES_API);
   EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attrs);
   EGLSurface surface = eglCreatePbufferSurface(display, config, surface_attrs);
   if (!context || !surface || !eglMakeCurrent(display, surface, surface, context)) return 5;
   printf("GL_RENDERER=%s\n", glGetString(GL_RENDERER));
   const char *vs_src = "attribute vec2 position; void main(){ gl_Position=vec4(position,0.,1.); }";
   const char *fs_src = "precision mediump float; void main(){ gl_FragColor=vec4(.25,.5,.75,1.); }";
   GLuint vs = shader(GL_VERTEX_SHADER, vs_src), fs = shader(GL_FRAGMENT_SHADER, fs_src);
   if (!vs || !fs) return 6;
   GLuint program = glCreateProgram();
   glAttachShader(program, vs); glAttachShader(program, fs);
   glBindAttribLocation(program, 0, "position"); glLinkProgram(program);
   GLint linked = 0; glGetProgramiv(program, GL_LINK_STATUS, &linked);
   if (!linked) return 7;
   glUseProgram(program);
   const GLfloat triangle[] = { -1.f,-1.f, 1.f,-1.f, 0.f,1.f };
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(GLfloat), triangle);
   glEnableVertexAttribArray(0);
   glDrawArrays(GL_TRIANGLES, 0, 3);
   glFinish();
   printf("RDMETAL_ORDINARY_TRIANGLE gl_error=0x%x\n", glGetError());
   eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
   eglDestroySurface(display, surface); eglDestroyContext(display, context);
   eglTerminate(display);
   return 0;
}
