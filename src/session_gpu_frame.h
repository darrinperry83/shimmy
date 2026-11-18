#ifndef SHIMMY_SESSION_GPU_FRAME_H
#define SHIMMY_SESSION_GPU_FRAME_H

#include <IOSurface/IOSurface.h>
#include <stdint.h>
#include <sys/types.h>

/* Host-only transport for a completed compositor frame. This does not perform
 * KMS synchronization: the caller must prove generation complete before send.
 * The callback receives a borrowed IOSurfaceRef; retain it until GPU present
 * completion if presentation is asynchronous. */
typedef struct {
  uint64_t high, low;
} session_gpu_epoch;

int session_gpu_epoch_parse(const char *text, session_gpu_epoch *out);
int session_gpu_epoch_format(session_gpu_epoch epoch, char out[33]);

typedef long (*session_gpu_present_fn)(IOSurfaceRef surface, uint32_t crtc,
                                       uint64_t generation, void *ctx);
typedef void (*session_gpu_lost_fn)(void *ctx);

/* PID 1 is the sole receiver. expected_pid must be the authorized compositor
 * host PID, learned from the session/DRM-master lifecycle, never from a frame.
 * expected_uid is the actual Darwin UID in its Mach audit token. */
int session_gpu_host_start(pid_t expected_pid, uid_t expected_uid,
                           session_gpu_epoch epoch,
                           session_gpu_present_fn present,
                           session_gpu_lost_fn lost, void *ctx);
/* Boot mode: accept only a process that proves possession of the exclusive
 * DRM-master flock open-file description. lock_path must be the host-side
 * private $RDSOCKETGROUP/drm-master.lock path. */
int session_gpu_host_start_from_master(uid_t expected_uid,
                                       session_gpu_epoch epoch,
                                       const char *lock_path,
                                       session_gpu_present_fn present,
                                       session_gpu_lost_fn lost, void *ctx);
/* A DRM-master child registers the actual native lock FD. The host learns its
 * PID from the Mach audit trailer, never from this function's arguments. */
int session_gpu_child_register_master(pid_t owner_pid, session_gpu_epoch epoch,
                                      int master_lock_fd, int drm_fd);
int session_gpu_child_unregister_master(pid_t owner_pid, session_gpu_epoch epoch,
                                        int master_lock_fd, int drm_fd);
/* Revoke the old producer and clear its frame; useful on logout/restart. */
int session_gpu_host_set_producer(pid_t expected_pid, uid_t expected_uid);
void session_gpu_host_stop(void);

/* width/height/format are validated against the actual IOSurface on receipt.
 * Format must be kCVPixelFormatType_32BGRA ('BGRA'). owner_pid and epoch come
 * from the trusted boot/session handoff, not from guest-controlled metadata. */
int session_gpu_child_present(pid_t owner_pid, session_gpu_epoch epoch,
                              IOSurfaceRef surface, uint64_t generation,
                              uint32_t crtc, uint32_t width, uint32_t height,
                              uint32_t format);
/* Full-init path: each frame also proves the caller still possesses the
 * current DRM-master lock. An earlier registration alone is not sufficient. */
int session_gpu_child_present_master(pid_t owner_pid, session_gpu_epoch epoch,
                                     int master_lock_fd, IOSurfaceRef surface,
                                     uint64_t generation, uint32_t crtc,
                                     uint32_t width, uint32_t height,
                                     uint32_t format);
/* A logind-handoff compositor presents with the exact DRM open file
 * description previously registered by the lock-owning logind process. */
int session_gpu_child_present_device(pid_t owner_pid, session_gpu_epoch epoch,
                                     int drm_fd, IOSurfaceRef surface,
                                     uint64_t generation, uint32_t crtc,
                                     uint32_t width, uint32_t height,
                                     uint32_t format);

#endif
