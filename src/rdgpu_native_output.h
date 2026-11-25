// rdgpu_native_output.h — native display geometry for the Wayland output advertisement.
//
// The Wayland compositor (wlsrv.c) advertises one wl_output whose mode/scale must match the
// physical display the guest's window actually lands on, otherwise Hyprland/KDE render a
// letterboxed (or oversized) desktop. These are auto-detected from NSScreen, not hardcoded.
#ifndef RDGPU_NATIVE_OUTPUT_H
#define RDGPU_NATIVE_OUTPUT_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

// Fill *w,*h with the native PIXEL size (round(frame.size * backingScaleFactor)) and *scale
// with the backing scale factor of guest output `index`. Index 0 is the primary output — the
// display the primary guest window sits on, falling back to NSScreen.mainScreen. Returns 0 and
// leaves the out-params unchanged (w/h == 0) when the requested output is unavailable.
int rdgpu_native_output(size_t index, uint32_t* w, uint32_t* h, double* scale);

// Geometry for the two virtual DRM connectors. Connector 1 follows the first
// online external display; connector 2 follows the online builtin display.
// Returns -1 when that display is not currently available. Unlike the nested
// Wayland output above, this mapping must not change when the guest window moves.
int rdgpu_drm_connector_output(uint32_t connector_id, uint32_t* w, uint32_t* h);

#ifdef __cplusplus
}
#endif
#endif
