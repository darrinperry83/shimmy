/* test_dmabuf_cross.c — two-process proof of the cross-process dma-buf import.
 *
 * Plays the plasmashell -> kwin split in miniature, entirely host-side:
 *
 *   exporter (plays plasmashell, a shim_cli process):
 *     MODE_CREATE_DUMB -> fill a non-black pattern via MODE_MAP_DUMB + drm_mmap_bo
 *     -> PRIME_HANDLE_TO_FD -> send the dma-buf fd to the importer via SCM_RIGHTS.
 *
 *   importer (plays kwin, a DIFFERENT shim_cli process):
 *     PRIME_FD_TO_HANDLE -> MODE_MAP_DUMB (must NOT return -EINVAL) -> drm_mmap_bo
 *     -> read the shared backing and verify the exact pattern (non-black). Also
 *     verifies drm_bo_map_by_fd (the present path) reads the same bytes.
 *
 * The two halves run in separate processes (fork), so each has its own g_bo[] /
 * g_export[] tables — the point is that the fd, not process-local state, carries
 * the pixels. Link against drmasahi.c + rdgpu_surface_handoff.c.
 *
 *   ./test_dmabuf_cross ; echo exit=$?
 */
#include "drmasahi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/wait.h>

#define DRM_IOCTL_PRIME_HANDLE_TO_FD 0xc00c642dULL
#define DRM_IOCTL_PRIME_FD_TO_HANDLE 0xc00c642eULL
#define DRM_IOCTL_MODE_CREATE_DUMB   0xc02064b2ULL
#define DRM_IOCTL_MODE_MAP_DUMB      0xc01064b3ULL

struct drm_prime_handle      { uint32_t handle; uint32_t flags; int32_t fd; };
struct drm_mode_create_dumb  { uint32_t height, width, bpp, flags; uint32_t handle, pitch; uint64_t size; };
struct drm_mode_map_dumb     { uint32_t handle, pad; uint64_t offset; };

#define W 64
#define H 64
#define PITCH 256          /* (W*4 + 63) & ~63 for W=64, bpp=32 */

static void fill_pattern(uint8_t* p) {
  for (uint32_t y = 0; y < H; y++)
    for (uint32_t x = 0; x < W; x++) {
      uint8_t* px = p + y * PITCH + x * 4;
      px[0] = (uint8_t)(x & 0xff);   /* B */
      px[1] = (uint8_t)(y & 0xff);   /* G */
      px[2] = 0x80;                  /* R */
      px[3] = 0xFF;                  /* A */
    }
}

static int verify_pattern(const uint8_t* p) {
  for (uint32_t y = 0; y < H; y++)
    for (uint32_t x = 0; x < W; x++) {
      const uint8_t* px = p + y * PITCH + x * 4;
      if (px[0] != (uint8_t)(x & 0xff) || px[1] != (uint8_t)(y & 0xff) ||
          px[2] != 0x80 || px[3] != 0xFF) {
        fprintf(stderr, "MISMATCH at %u,%u: got %02x %02x %02x %02x\n",
                x, y, px[0], px[1], px[2], px[3]);
        return 0;
      }
    }
  return 1;
}

static int send_fd(int sock, int fd) {
  char control[CMSG_SPACE(sizeof(int))] = {0};
  char byte = 'x';
  struct iovec iov = { &byte, 1 };
  struct msghdr msg = {0};
  msg.msg_iov = &iov; msg.msg_iovlen = 1;
  msg.msg_control = control; msg.msg_controllen = sizeof control;
  struct cmsghdr* cm = CMSG_FIRSTHDR(&msg);
  cm->cmsg_level = SOL_SOCKET; cm->cmsg_type = SCM_RIGHTS;
  cm->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(cm), &fd, sizeof(int));
  return sendmsg(sock, &msg, 0) < 0 ? -1 : 0;
}

static int recv_fd(int sock) {
  char control[CMSG_SPACE(sizeof(int))] = {0};
  char byte = 0;
  struct iovec iov = { &byte, 1 };
  struct msghdr msg = {0};
  msg.msg_iov = &iov; msg.msg_iovlen = 1;
  msg.msg_control = control; msg.msg_controllen = sizeof control;
  if (recvmsg(sock, &msg, 0) < 0) return -1;
  int fd = -1;
  for (struct cmsghdr* cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm))
    if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS)
      memcpy(&fd, CMSG_DATA(cm), sizeof(int));
  return fd;
}

static int do_exporter(int sock) {
  struct drm_mode_create_dumb c = {0};
  c.width = W; c.height = H; c.bpp = 32;
  long r = drm_ioctl(0, DRM_IOCTL_MODE_CREATE_DUMB, (unsigned long)&c);
  if (r || !c.handle) { fprintf(stderr, "EXPORTER create_dumb failed r=%ld\n", r); return 1; }
  if (c.pitch != PITCH) { fprintf(stderr, "EXPORTER pitch=%u (expected %u)\n", c.pitch, PITCH); return 1; }

  struct drm_mode_map_dumb m = { c.handle, 0, 0 };
  r = drm_ioctl(0, DRM_IOCTL_MODE_MAP_DUMB, (unsigned long)&m);
  if (r || m.offset == 0) { fprintf(stderr, "EXPORTER map_dumb failed r=%ld off=%llu\n", r, (unsigned long long)m.offset); return 1; }
  uint8_t* p = (uint8_t*)drm_mmap_bo(0, (long)m.offset);
  if (!p) { fprintf(stderr, "EXPORTER mmap_bo NULL\n"); return 1; }
  fill_pattern(p);

  struct drm_prime_handle ph = { c.handle, 0, -1 };
  r = drm_ioctl(0, DRM_IOCTL_PRIME_HANDLE_TO_FD, (unsigned long)&ph);
  if (r || ph.fd < 0) { fprintf(stderr, "EXPORTER prime_handle_to_fd failed r=%ld fd=%d\n", r, ph.fd); return 1; }
  fprintf(stderr, "EXPORTER create_dumb handle=0x%x pitch=%u -> dmabuf fd=%d (shared backing)\n",
          c.handle, c.pitch, ph.fd);

  /* In-process "own export" round-trip must still return the ORIGINAL handle. */
  {
    struct drm_prime_handle own = { 0, 0, ph.fd };
    r = drm_ioctl(0, DRM_IOCTL_PRIME_FD_TO_HANDLE, (unsigned long)&own);
    if (r || own.handle != c.handle) {
      fprintf(stderr, "EXPORTER own-export round-trip failed r=%ld got=0x%x want=0x%x\n",
              r, own.handle, c.handle);
      return 1;
    }
    fprintf(stderr, "EXPORTER own-export round-trip handle=0x%x OK\n", own.handle);
  }

  if (send_fd(sock, ph.fd)) { perror("sendmsg"); return 1; }
  close(ph.fd);

  char ack = 0;
  if (read(sock, &ack, 1) != 1) { fprintf(stderr, "EXPORTER no ack\n"); return 1; }
  return ack == 'P' ? 0 : 1;
}

static int do_importer(int sock) {
  int fd = recv_fd(sock);
  if (fd < 0) { fprintf(stderr, "IMPORTER no fd\n"); return 1; }

  struct drm_prime_handle ph = { 0, 0, fd };
  long r = drm_ioctl(0, DRM_IOCTL_PRIME_FD_TO_HANDLE, (unsigned long)&ph);
  if (r || !ph.handle) { fprintf(stderr, "IMPORTER prime_fd_to_handle failed r=%ld\n", r); return 1; }
  uint32_t handle = ph.handle;

  /* MODE_MAP_DUMB must return a valid offset now (previously -EINVAL -> null deref). */
  struct drm_mode_map_dumb m = { handle, 0, 0 };
  r = drm_ioctl(0, DRM_IOCTL_MODE_MAP_DUMB, (unsigned long)&m);
  if (r || m.offset == 0) {
    fprintf(stderr, "IMPORTER map_dumb FAILED r=%ld off=%llu (null-deref would follow)\n",
            r, (unsigned long long)m.offset);
    char ack = 'F'; write(sock, &ack, 1);
    return 1;
  }

  uint8_t* p = (uint8_t*)drm_mmap_bo(0, (long)m.offset);
  if (!p) {
    fprintf(stderr, "IMPORTER mmap_bo NULL\n");
    char ack = 'F'; write(sock, &ack, 1);
    return 1;
  }
  int egl_ok = verify_pattern(p);

  size_t sz = 0;
  uint8_t* q = (uint8_t*)drm_bo_map_by_fd(fd, &sz);
  int present_ok = q ? verify_pattern(q) : 0;

  fprintf(stderr, "IMPORTER fd=%d -> handle=0x%x off=0x%llx egl_ok=%d present_ok=%d size=%zu\n",
          fd, handle, (unsigned long long)m.offset, egl_ok, present_ok, sz);

  char ack = (egl_ok && present_ok) ? 'P' : 'F';
  write(sock, &ack, 1);
  close(fd);
  return (egl_ok && present_ok) ? 0 : 1;
}

int main(void) {
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) { perror("socketpair"); return 2; }

  pid_t pid = fork();
  if (pid < 0) { perror("fork"); return 2; }
  if (pid == 0) {
    close(sv[0]);
    int rc = do_importer(sv[1]);
    _exit(rc);
  }
  close(sv[1]);
  int prc = do_exporter(sv[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  int crc = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
  int rc = (prc == 0 && crc == 0) ? 0 : 1;
  fprintf(stderr, "RESULT exporter=%d importer=%d -> %s\n", prc, crc, rc ? "FAIL" : "PASS");
  return rc;
}
