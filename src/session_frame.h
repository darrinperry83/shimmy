#ifndef SHIMMY_SESSION_FRAME_H
#define SHIMMY_SESSION_FRAME_H

#include <stdint.h>

typedef long (*session_frame_present_fn)(const uint8_t* bgra, uint32_t width,
                                         uint32_t height, uint32_t crtc);

// PID 1 starts the receiver after its private /tmp/rdx-<pid> namespace exists.
// It is the only process allowed to call the host window presenter.
int session_frame_host_start(session_frame_present_fn present);
int session_frame_host_start_with_lost(session_frame_present_fn present,
                                       void (*lost)(void));
void session_frame_host_stop(void); // tests and orderly shutdown; boot reaper handles crashes

// The native compositor calls this on each DRM flip. A stable CPU framebuffer
// snapshot is published to PID 1 without creating a child AppKit window.
long session_frame_child_present(const uint8_t* bgra, uint32_t width,
                                 uint32_t height, uint32_t crtc);

#endif
