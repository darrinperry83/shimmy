#ifndef SHIMMY_DRM_OUTPUT_ROUTE_H
#define SHIMMY_DRM_OUTPUT_ROUTE_H

// Connector 1 is external DP / CRTC 1; connector 2 is builtin eDP / CRTC 2.
// The single connected output must use the existing primary Mac window.
enum drm_output_route { DRM_ROUTE_NONE, DRM_ROUTE_PRIMARY, DRM_ROUTE_SECONDARY };
static inline enum drm_output_route drm_output_route(unsigned crtc, int external, int builtin) {
  if (crtc == 1) return external ? DRM_ROUTE_PRIMARY : DRM_ROUTE_NONE;
  if (crtc == 2) return builtin ? (external ? DRM_ROUTE_SECONDARY : DRM_ROUTE_PRIMARY)
                                : DRM_ROUTE_NONE;
  return DRM_ROUTE_NONE;
}
#endif
