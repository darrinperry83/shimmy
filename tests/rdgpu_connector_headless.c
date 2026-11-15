#include <stdint.h>
// The headless CLI has no AppKit display inventory. Preserve its diagnostic modes.
int rdgpu_drm_connector_output(uint32_t connector_id, uint32_t* width, uint32_t* height) {
    if (!width || !height) return -1;
    if (connector_id == 1) { *width = 3440; *height = 1440; return 0; }
    if (connector_id == 2) { *width = 3600; *height = 2338; return 0; }
    return -1;
}
