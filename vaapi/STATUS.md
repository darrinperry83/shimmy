# VAAPI → VideoToolbox bridge: status & continuation notes

Last updated: 2026-11-14. This is a handoff note for the next session.

## What works (validated)

**Single-frame H.264 decode via ffmpeg VAAPI is byte-exact.**

```
./shim_cli ../rootfs-arch /usr/bin/ffmpeg -hide_banner -loglevel error \
  -hwaccel vaapi -hwaccel_output_format nv12 -i /root/test.h264 -f null -
# -> frame=1, no "Failed to initialise VAAPI"

./shim_cli ../rootfs-arch /usr/bin/ffmpeg -hide_banner -loglevel error \
  -hwaccel vaapi -hwaccel_output_format nv12 -i /root/test.h264 \
  -f rawvideo -pix_fmt nv12 - > /tmp/nv12.bin
# 115200 bytes, FNV-1a = 0xbf6744e3cfd7e9eb (matches the rdva_probe reference decode)
```

The whole ffmpeg surface-plumbing path now works end to end:

- `vaQuerySurfaceAttributes` returns `VASurfaceAttribPixelFormat=NV12`, `MemoryType=VA`, and
  min/max dims (this was the original "Hardware does not offer any usable surface formats"
  blocker).
- `vaCreateSurfaces`/`vaCreateSurfaces2`, `vaMapBuffer`/`vaMapBuffer2`, `vaSyncSurface`/
  `vaSyncSurface2`, `vaGetImage`/`vaPutImage`, `vaCreateImage`/`vaDeriveImage` are all
  implemented and wired in the vtable.
- `vaRenderPicture` now handles the **real** ffmpeg buffer types: `VAPictureParameterBufferType`
  (→ reconstructs SPS+PPS NALs from `VAPictureParameterBufferH264`), `VASliceDataBufferType`
  (→ prepends a start code; the slice NAL is already in bitstream form), and ignores
  `VAIQMatrixBufferType`/`VASliceParameterBufferType`.
- `vaEndPicture` **feeds** the reconstructed Annex-B access unit to the host (decode order);
  `vaSyncSurface` **pulls** one decoded NV12 frame back (display order).

## The bridge contract (syscall 0x2003)

`rdva_host.h` now has four ops:

- `RDVA_OP_QUERY` — presence/version handshake.
- `RDVA_OP_DECODE(a1=bits, a2=len, a3=poc)` — feed one access unit. `poc` (frame POC) is the
  VideoToolbox PTS so B-frames reorder correctly.
- `RDVA_OP_GET_FRAME(a1=out, a2=cap, a3=&written)` — pop the next display-order NV12 frame.
- `RDVA_OP_RESET` — invalidate the cached session + drop the FIFO (guest `vaTerminate`).

The host (`src/rdva_host.m`) keeps a **persistent** `VTDecompressionSession` keyed on the
SPS/PPS bytes, feeds frames with DTS = monotonic decode counter / PTS = POC, and buffers outputs
in a small FIFO that the guest drains in display order.

## Multi-frame: current state (incomplete)

`test_p.h264` (P-only, 320x240, 50 frames, `-bf 0`) and `test_multi.h264` (B-frames, `-bf 2`)
are in `rootfs-arch/root/` for testing.

Three root causes were found and fixed along the way:

1. **Frame-threading race.** ffmpeg's h264 hwaccel sets `HWACCEL_CAP_ASYNC_SAFE`, so the driver
   is entered from multiple frame threads + the output thread concurrently. The shared
   `next_*_id` counters were a read-modify-write race → the `used=0` corruption. Fixed with
   `__atomic_fetch_add` (`rdva_next`) and a thread-local `g_cur_surface` (the old per-context
   `current_surface` was also raced).
2. **POC units.** ffmpeg stores POC in "field units" (2× the frame POC for progressive), so the
   PTS had gaps that made VideoToolbox hold P frames. Fixed by passing `TopFieldOrderCnt / 2`.
3. **Reorder-buffer hold (NOT yet fixed).** With the first two fixed, P-only decodes frames 0,1
   then stalls: VideoToolbox holds the 3rd P frame in its reorder buffer and
   `VTDecompressionSessionWaitForAsynchronousFrames` does not flush it. `rdva_get_frame` now
   calls `VTDecompressionSessionFinishDelayedFrames` on underflow to force it out — **this
   change is in place but has not been re-tested** (see "next steps").

## Debugging convention

- Guest driver (`vaapi/rdva_drv_video.c`): traces gated by `RDVA_TRACE` in the guest
  → set `RDENV_RDVA_TRACE=1` on the shim command line.
- Host (`src/rdva_host.m`): traces gated by `RDVA_TRACE` on the shim itself
  → set `RDVA_TRACE=1` (host-side, not `RDENV_`).

## Next steps

1. P-only still stalls at the 3rd frame after the `VTDecompressionSessionFinishDelayedFrames`
   underflow flush (re-tested 2026-11-14: still "Failed to sync surface"). The hold is not the
   end-of-stream reorder buffer — VideoToolbox is genuinely not emitting the 3rd P frame even
   though DTS==PTS. Next: isolate whether it is the reconstructed SPS (`max_num_ref_frames`,
   profile/level, missing VUI) or the DTS/PTS scheme (try DTS=PTS for P-only, or drop the
   `RequireHardwareAcceleratedVideoDecoder` / IOSurface-backed destination).
2. B-frame streams (`test_multi.h264`): verify the POC-based PTS reorders correctly. The FIFO
   underflow in the B-frame case may still need `FinishDelayedFrames` at end-of-stream only
   (calling it on *every* underflow flushes in decode order, which is wrong for B frames).
3. `vaExportSurfaceHandle` dma-buf zero-copy, encode, and HEVC/VP9/AV1 remain unimplemented.

## Files touched

- `vaapi/rdva_drv_video.c` — guest backend (SPS/PPS reconstruction + surface plumbing).
- `src/rdva_host.h` / `src/rdva_host.m` — host VideoToolbox bridge (persistent session + FIFO).
- `vaapi/build.sh` — unchanged (rebuild + install into `../rootfs-arch`).

Not modified: `/Users/dperry/linux-shim`, `shim_gui`.
