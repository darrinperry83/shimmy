// rdva_host.h — the media-engine bridge: guest VAAPI (libva) decode -> Apple VideoToolbox.
//
// The guest's libva backend (rdva_drv_video.so, cross-compiled into the rootfs under
// /usr/lib/dri) advertises H.264 decode and, instead of touching any Linux DRM/VAAPI
// hardware, issues a private Linux syscall (0x2003) that this shim answers in-process.
// This is the SAME shape as the rdmetal GPU broker (syscall 0x2002 -> rdgpu_mock_gate.c)
// and the audio bridge (snd_alsa.c -> coreaudio_host.c): the guest half marshals a request
// into registers, the host half fulfils it with an Apple framework.
//
// Here the Apple framework is VideoToolbox: rdva_bridge() keeps a persistent
// VTDecompressionSession (keyed on the SPS/PPS), decodes each Annex-B H.264 access unit the
// guest reconstructs, and hands the decoded NV12 frames back in display order.  The
// "zero-copy IOSurface -> guest dma-buf" hand-back is deliberately a follow-up.
#ifndef RDVA_HOST_H
#define RDVA_HOST_H
#include <stdint.h>

// Guest-side private syscall number (aarch64 x8).  0x2002 is the rdmetal GPU broker;
// 0x2003 is reserved here for VAAPI/VideoToolbox.
#define RDVA_GUEST_SYSCALL 0x2003u

// op codes (a0)
#define RDVA_OP_DECODE    1u   // feed one Annex-B H.264 access unit (decode order)
#define RDVA_OP_QUERY     2u   // report host bridge presence/version (handshake)
#define RDVA_OP_GET_FRAME 3u   // pull the next display-order decoded NV12 frame
#define RDVA_OP_RESET     4u   // invalidate the cached session + drop queued frames

// rdva_bridge(op, a1, a2, a3, a4, a5)
//   RDVA_OP_DECODE:
//     a1 = guest ptr to Annex-B H.264 access unit (SPS + PPS + slice NAL(s))
//     a2 = access unit length (bytes)
//     a3 = picture order count (display order), used as the VideoToolbox PTS for B-frame reordering
//     returns 0 on success, else negative errno.
//   RDVA_OP_GET_FRAME:
//     a1 = guest ptr to output buffer (w*h*3/2 bytes, NV12: Y plane then interleaved UV)
//     a2 = output buffer capacity (bytes)
//     a3 = guest ptr to int64_t -> host writes decoded byte count (or 0 on failure)
//     returns 0 on success, -EAGAIN if no frame is ready, else negative errno.
//   RDVA_OP_QUERY:
//     returns a positive bridge version (1) when the media-engine bridge is compiled in.
//   RDVA_OP_RESET:
//     drops the cached session and queued frames; returns 0.
long rdva_bridge(uint64_t op, uint64_t a1, uint64_t a2, uint64_t a3,
                 uint64_t a4, uint64_t a5);

#endif
