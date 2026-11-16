#include "drm_output_route.h"
#include <assert.h>

int main(void) {
  assert(drm_output_route(1, 1, 1) == DRM_ROUTE_PRIMARY);
  assert(drm_output_route(2, 1, 1) == DRM_ROUTE_SECONDARY);
  assert(drm_output_route(1, 1, 0) == DRM_ROUTE_PRIMARY);
  assert(drm_output_route(2, 1, 0) == DRM_ROUTE_NONE);
  assert(drm_output_route(1, 0, 1) == DRM_ROUTE_NONE);
  assert(drm_output_route(2, 0, 1) == DRM_ROUTE_PRIMARY);
  assert(drm_output_route(1, 0, 0) == DRM_ROUTE_NONE);
  assert(drm_output_route(2, 0, 0) == DRM_ROUTE_NONE);
  return 0;
}
