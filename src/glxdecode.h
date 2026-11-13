// glxdecode.h — GLX indirect-rendering wire decoder.
//
// The X server (x11srv.c) hands us the payload of the GLX rendering requests; we walk the
// batched GL command stream and drive glbridge (glbridge.h), which rasterizes it with Metal
// into an offscreen texture.  On SwapBuffers the finished frame is read back into the GL
// window's CPU framebuffer so the ordinary x11rnd_present() path puts it on screen.
//
// Wire format (GLX 1.4 protocol, "rendering commands"):
//   GLXRender      = { CARD32 contextTag } followed by N commands, each
//                    { CARD16 length; CARD16 opcode; payload } where `length` counts the
//                    WHOLE command including its 4-byte header and is a multiple of 4.
//   GLXRenderLarge = { CARD32 contextTag; CARD16 requestNumber; CARD16 requestTotal;
//                      CARD32 dataBytes; data }.  The concatenation of every chunk's data is
//                    ONE oversized command with an 8-byte header { CARD32 length; CARD32 op }
//                    (length again counts the whole command, header included).
#ifndef RDRIFT_GLXDECODE_H
#define RDRIFT_GLXDECODE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// The connection's byte order, from the X setup request ('l' = LSBFirst, 'B' = MSBFirst).
// Every multi-byte field in the GL stream is read in this order.  Default: LSBFirst.
void glxd_set_byte_order(int lsb_first);

// Make sure the GL bridge exists and its framebuffer is w*h.  Cheap when already correct.
// Returns 0, or a negative errno (in which case every other entry point is a no-op).
int  glxd_ensure(int w, int h);

// Decode a GLXRender payload.  `p`/`n` are the bytes AFTER the 4-byte contextTag.
void glxd_render(const uint8_t* p, size_t n);

// Feed one GLXRenderLarge request: `d`/`dlen` are the bytes after the 4-byte X request
// header, i.e. starting at contextTag.  Dispatches once the last chunk arrives.
void glxd_render_large(const uint8_t* d, size_t dlen);

// --- GL "single" (non-Render) commands the decoder has to see ---------------------------
uint32_t glxd_gen_lists(uint32_t range);
void     glxd_new_list(uint32_t list, uint32_t mode);
void     glxd_end_list(void);
void     glxd_delete_lists(uint32_t list, uint32_t range);
int      glxd_is_list(uint32_t list);
int      glxd_is_enabled(uint32_t cap);

// End the frame.  When `rgba` is non-NULL the finished frame is also read back into it
// (w*h*4, top-left origin, R first) with alpha forced opaque.
void glxd_swap(uint8_t* rgba, int w, int h);

// Non-zero once at least one GL rendering command has been decoded.
int  glxd_has_content(void);

// Total rendering commands decoded (diagnostics).
uint64_t glxd_command_count(void);

#ifdef __cplusplus
}
#endif
#endif /* RDRIFT_GLXDECODE_H */
