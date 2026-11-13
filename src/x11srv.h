// x11srv.h — in-process X11 server: X protocol -> CPU framebuffer -> Metal window.
#ifndef RDRIFT_X11SRV_H
#define RDRIFT_X11SRV_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// Spawn the listener thread on a unix socket (the guest's /tmp/.X11-unix/X0 is redirected here
// by net_set_unix_prefix). Returns 0 on success. Single client is sufficient.
int  x11srv_start(const char* socket_path);
void x11srv_stop(void);

// Re-establish the server inside a fork() child, on a socket keyed to the child's own pid.
// See wlsrv_restart_after_fork(); x11srv_stop() cannot be used here (it joins a thread that fork
// did not copy, and unlinks the parent's socket).
int  x11srv_restart_after_fork(const char* socket_path);

// --- render bridge (x11srv_render.m): thin forwarders to the gpu module ---
void x11rnd_open(uint32_t w, uint32_t h);
void x11rnd_present(const uint8_t* rgba, uint32_t w, uint32_t h);
// Same, but the framebuffer is B,G,R,A in memory — which is what a wl_shm ARGB8888/XRGB8888
// buffer already is, and what MTLPixelFormatBGRA8Unorm consumes with no conversion at all.
//
// THIS IS A SEPARATE ENTRY POINT ON PURPOSE. The obvious implementation — flip the staging
// texture to BGRA once, globally — swaps red and blue for every X11 client, because x11srv.c's
// framebuffer is genuinely RGBA (src/x11srv.c:134, "byte0=R .. byte3=A"). No Wayland gate can see
// that, so the byte order is stated by each caller rather than assumed by the window.
void x11rnd_present_bgra(const uint8_t* bgra, uint32_t w, uint32_t h);
int  x11rnd_poll(void);
// Live cursor for QueryPointer (window-local, top-left origin).
void x11srv_pointer(int* x, int* y);

#ifdef __cplusplus
}
#endif
#endif
