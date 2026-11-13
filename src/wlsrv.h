// wlsrv.h — in-process Wayland compositor: Wayland protocol -> CPU framebuffer -> Metal window.
//
// The counterpart to x11srv.h. Where the X11 server has to rasterize drawing requests itself,
// a Wayland compositor never draws: the client renders into shared memory (Mesa's swrast does
// exactly this) and hands us a wl_buffer. So this module is protocol plumbing plus one blit.
//
// That is also why Wayland is the better host for modern GL here: EGL's Wayland platform goes
// straight to wl_shm, so the guest gets the full GL 3.3 / GLES 3.1 Mesa stack, while indirect
// GLX is stuck at fixed-function 1.4 forever (see ARCHITECTURE.md).
#ifndef RDRIFT_WLSRV_H
#define RDRIFT_WLSRV_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// Spawn the listener thread on a unix socket. The guest's $XDG_RUNTIME_DIR/wayland-0 is
// redirected here by net_set_unix_prefix(). Returns 0 on success. One client is sufficient.
int  wlsrv_start(const char* socket_path);
void wlsrv_stop(void);

// Re-establish the compositor inside a fork() child, on a socket of the child's own. fork() keeps
// only the calling thread, so the listener thread is gone in the child while its descriptors and
// state remain — a display connection nothing services. Pass a path in a namespace keyed to the
// CHILD's pid: reusing the inherited path would unlink it out from under the parent.
int  wlsrv_restart_after_fork(const char* socket_path);

#ifdef __cplusplus
}
#endif
#endif
