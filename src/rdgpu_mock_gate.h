#ifndef RDGPU_MOCK_GATE_H
#define RDGPU_MOCK_GATE_H
#include <stdint.h>
#include <IOSurface/IOSurface.h>
long rdgpu_mock_gate(uint64_t op,uint64_t a1,uint64_t a2,uint64_t a3,
                     uint64_t a4,uint64_t a5);
/* Resolve a received dma-buf carrier fd to the in-process metal fixture's
 * retained IOSurface by matching fstat(st_dev, st_ino) against each issued
 * carrier. Returns NULL when the fixture is inactive (socket broker / a
 * different process) or the fd matches no issued carrier. The returned
 * reference is retained by the fixture; a caller that must outlive the
 * fixture should CFRetain it. */
IOSurfaceRef rdgpu_mock_gate_resolve_fd(int fd);
#endif
