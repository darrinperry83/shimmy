#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stddef.h>
static GLuint shader(GLenum kind,const char *source) {
 GLuint s=glCreateShader(kind);glShaderSource(s,1,&source,0);glCompileShader(s);
 GLint ok=0;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);return ok?s:0;
}
int main(void) {
 PFNEGLGETPLATFORMDISPLAYEXTPROC platform=(void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
 if(!platform)return 1;
 EGLDisplay d=platform(EGL_PLATFORM_SURFACELESS_MESA,EGL_DEFAULT_DISPLAY,0);
 if(!d || !eglInitialize(d,0,0))return 2;
 EGLint attrs[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,
 EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_DEPTH_SIZE,24,EGL_STENCIL_SIZE,8,EGL_NONE};
 EGLConfig config;EGLint count=0;if(!eglChooseConfig(d,attrs,&config,1,&count)||!count)return 3;
 EGLint ca[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE},sa[]={EGL_WIDTH,96,EGL_HEIGHT,80,EGL_NONE};
 eglBindAPI(EGL_OPENGL_ES_API);
 EGLContext ctx=eglCreateContext(d,config,EGL_NO_CONTEXT,ca);
 EGLSurface surface=eglCreatePbufferSurface(d,config,sa);
 if(!ctx||!surface||!eglMakeCurrent(d,surface,surface,ctx))return 4;
 const char *vs="attribute vec2 position; attribute vec4 color; attribute float opacity; varying vec4 v_color; varying float v_opacity; void main(){gl_Position=vec4(position,0.,1.);v_color=color;v_opacity=opacity;}";
 const char *fs="precision mediump float; varying vec4 v_color; varying float v_opacity; void main(){gl_FragColor=v_color*v_opacity;}";
 GLuint v=shader(GL_VERTEX_SHADER,vs),f=shader(GL_FRAGMENT_SHADER,fs);if(!v||!f)return 5;
 GLuint program=glCreateProgram();glAttachShader(program,v);glAttachShader(program,f);
 glBindAttribLocation(program,0,"position");glBindAttribLocation(program,1,"color");glBindAttribLocation(program,2,"opacity");glLinkProgram(program);
 GLint ok=0;glGetProgramiv(program,GL_LINK_STATUS,&ok);if(!ok)return 6;glUseProgram(program);
 struct Vertex {float x,y;unsigned char rgba[4];float opacity;} vertices[]={
 {-1,-1,{255,0,0,255},1},{1,-1,{255,0,0,255},1},{0,1,{255,0,0,255},1}};
 glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,sizeof vertices[0],vertices);
 glVertexAttribPointer(1,4,GL_UNSIGNED_BYTE,GL_TRUE,sizeof vertices[0],(char*)vertices+offsetof(struct Vertex,rgba));
 glVertexAttribPointer(2,1,GL_FLOAT,GL_FALSE,sizeof vertices[0],(char*)vertices+offsetof(struct Vertex,opacity));
 for(int i=0;i<3;i++)glEnableVertexAttribArray(i);
 glViewport(0,0,96,80);glClearColor(0,0,0,1);glClearDepthf(1);glClearStencil(0);
 glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
 glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LESS);glDrawArrays(GL_TRIANGLES,0,3);glFinish();
 if(glGetError()!=GL_NO_ERROR)return 7;
 puts("DEPTH_PHASE_RED");fflush(stdout);
 for(int i=0;i<3;i++){vertices[i].rgba[0]=0;vertices[i].rgba[1]=255;}
 glDrawArrays(GL_TRIANGLES,0,3);glFinish();if(glGetError()!=GL_NO_ERROR)return 8;
 puts("DEPTH_PHASE_OCCLUDED");fflush(stdout);
 glDepthFunc(GL_ALWAYS);glEnable(GL_STENCIL_TEST);glStencilFunc(GL_EQUAL,1,255);glStencilOp(GL_KEEP,GL_KEEP,GL_KEEP);
 glDrawArrays(GL_TRIANGLES,0,3);glFinish();if(glGetError()!=GL_NO_ERROR)return 9;
 puts("STENCIL_PHASE_REJECTED");fflush(stdout);
 glClearStencil(1);glClear(GL_STENCIL_BUFFER_BIT);glDrawArrays(GL_TRIANGLES,0,3);glFinish();
 if(glGetError()!=GL_NO_ERROR)return 10;
 puts("DEPTH_STENCIL_EGL_OK");fflush(stdout);
 /* Qt glyphs use alpha-only textures and position/UV/opacity attributes. */
 const char *uvvs="attribute vec2 position; attribute vec2 uv; attribute float opacity; uniform mat4 transform; varying vec2 v_uv; varying float v_opacity; void main(){gl_Position=transform*vec4(position,0.,1.);v_uv=uv;v_opacity=opacity;}";
 const char *uvfs="precision mediump float; uniform sampler2D glyph; uniform vec4 inkColor; uniform float gamma; varying vec2 v_uv; varying float v_opacity; void main(){gl_FragColor=inkColor*vec4(1.,1.,1.,clamp(pow(texture2D(glyph,v_uv).a*v_opacity,gamma),0.,1.));}";
 GLuint uvprogram=glCreateProgram();glAttachShader(uvprogram,shader(GL_VERTEX_SHADER,uvvs));glAttachShader(uvprogram,shader(GL_FRAGMENT_SHADER,uvfs));
 glBindAttribLocation(uvprogram,0,"position");glBindAttribLocation(uvprogram,1,"uv");glBindAttribLocation(uvprogram,2,"opacity");glLinkProgram(uvprogram);
 glGetProgramiv(uvprogram,GL_LINK_STATUS,&ok);if(!ok)return 11;glUseProgram(uvprogram);glUniform4f(glGetUniformLocation(uvprogram,"inkColor"),0,1,0,1);glUniform1f(glGetUniformLocation(uvprogram,"gamma"),2);
 float transform[16]={.5,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
 glUniformMatrix4fv(glGetUniformLocation(uvprogram,"transform"),1,GL_FALSE,transform);
 struct UVVertex {float x,y,u,v,opacity;} uvvertices[]={
 {-1,-1,0,0,1},{1,-1,1,0,1},{-1,1,0,1,1},{1,1,1,1,1}};
 glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,sizeof uvvertices[0],uvvertices);
 glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,sizeof uvvertices[0],(char*)uvvertices+8);
 glVertexAttribPointer(2,1,GL_FLOAT,GL_FALSE,sizeof uvvertices[0],(char*)uvvertices+16);
 GLuint tex;glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D,tex);
 glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
 glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
 unsigned char blank[16]={0},ink[4]={255,255,255,255};glPixelStorei(GL_UNPACK_ALIGNMENT,1);
 glTexImage2D(GL_TEXTURE_2D,0,GL_ALPHA,4,4,0,GL_ALPHA,GL_UNSIGNED_BYTE,blank);
 glTexSubImage2D(GL_TEXTURE_2D,0,1,1,2,2,GL_ALPHA,GL_UNSIGNED_BYTE,ink);
 glDisable(GL_DEPTH_TEST);glDisable(GL_STENCIL_TEST);glClear(GL_COLOR_BUFFER_BIT);
 unsigned short strip_indices[]={0,1,2,3};
 glDrawElements(GL_TRIANGLE_STRIP,4,GL_UNSIGNED_SHORT,strip_indices);glFinish();if(glGetError()!=GL_NO_ERROR)return 12;
 unsigned short fan_indices[]={0,1,3,2};glClear(GL_COLOR_BUFFER_BIT);
 glDrawElements(GL_TRIANGLE_FAN,4,GL_UNSIGNED_SHORT,fan_indices);glFinish();
 if(glGetError()!=GL_NO_ERROR)return 13;
 puts("ALPHA_GLYPH_EGL_OK");fflush(stdout);
 eglMakeCurrent(d,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
 eglDestroySurface(d,surface);eglDestroyContext(d,ctx);eglTerminate(d);return 0;
}
