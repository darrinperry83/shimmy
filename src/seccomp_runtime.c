#include "seccomp_runtime.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SHIM_SECCOMP_WIRE_MAGIC 0x53434631u /* SCF1 */
#define SHIM_SECCOMP_MAX_INSNS 256u

struct shim_seccomp_state {
  unsigned count;
  int no_new_privs;
  struct shim_sock_filter program[SHIM_SECCOMP_MAX_INSNS];
};

struct shim_seccomp_wire_header {
  uint32_t magic, version, count, no_new_privs;
};

struct shim_seccomp_state *shim_seccomp_state_new(
    const struct shim_sock_filter *program, unsigned count, int no_new_privs) {
  if (count > SHIM_SECCOMP_MAX_INSNS || (count && (!no_new_privs ||
      shim_seccomp_bpf_verify(program, count) != 0)) ||
      (!count && !no_new_privs)) return NULL;
  struct shim_seccomp_state *s = calloc(1, sizeof *s);
  if (!s) return NULL;
  s->count = count;
  s->no_new_privs = 1;
  if (count) memcpy(s->program, program, count * sizeof *program);
  return s;
}

int shim_seccomp_state_no_new_privs(const struct shim_seccomp_state *s) {
  return s ? s->no_new_privs : 0;
}

int shim_seccomp_state_check(const struct shim_seccomp_state *s,
                            uint32_t nr, const uint64_t args[6],
                            uint64_t instruction_pointer, long *result) {
  if (!s || !s->count) return 0;
  if (!args || !result) { if (result) *result = -1; return 1; }
  struct shim_seccomp_data data = {
    .nr = (int32_t)nr, .arch = SHIM_SECCOMP_ARCH_AARCH64,
    .instruction_pointer = instruction_pointer,
  };
  memcpy(data.args, args, sizeof data.args);
  uint32_t action = shim_seccomp_bpf_eval_verified(s->program, s->count, &data);
  if (action == SHIM_SECCOMP_RET_ALLOW) return 0;
  *result = -(long)(action & 0xffffu);
  return 1;
}

static int write_all(int fd, const void *data, size_t size) {
  const unsigned char *p = data;
  while (size) {
    ssize_t n = write(fd, p, size);
    if (n <= 0) return -1;
    p += n; size -= (size_t)n;
  }
  return 0;
}

static int read_all(int fd, void *data, size_t size) {
  unsigned char *p = data;
  while (size) {
    ssize_t n = read(fd, p, size);
    if (n <= 0) return -1;
    p += n; size -= (size_t)n;
  }
  return 0;
}

int shim_seccomp_state_export_fd(const struct shim_seccomp_state *s) {
  if (!s) return -1;
  int fds[2];
  if (pipe(fds) != 0) return -1;
  if (fcntl(fds[1], F_SETFL, O_NONBLOCK) != 0) {
    close(fds[0]); close(fds[1]); return -1;
  }
  struct shim_seccomp_wire_header h = {
    SHIM_SECCOMP_WIRE_MAGIC, 1, s->count, (uint32_t)s->no_new_privs
  };
  size_t bytes = s->count * sizeof s->program[0];
  int ok = write_all(fds[1], &h, sizeof h) == 0 &&
           write_all(fds[1], s->program, bytes) == 0;
  close(fds[1]);
  if (!ok) { close(fds[0]); return -1; }
  return fds[0];
}

struct shim_seccomp_state *shim_seccomp_state_import_fd(int fd) {
  struct stat st;
  int flags = fcntl(fd, F_GETFL);
  if (fd < 0 || flags < 0 || (flags & O_ACCMODE) != O_RDONLY ||
      fstat(fd, &st) != 0 || !S_ISFIFO(st.st_mode)) return NULL;
  /* A forged or damaged inherited pipe must fail, not hang guest startup. */
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) return NULL;
  struct shim_seccomp_wire_header h;
  if (read_all(fd, &h, sizeof h) != 0 ||
      h.magic != SHIM_SECCOMP_WIRE_MAGIC || h.version != 1 ||
      h.count > SHIM_SECCOMP_MAX_INSNS || h.no_new_privs != 1) return NULL;
  struct shim_sock_filter program[SHIM_SECCOMP_MAX_INSNS];
  if (read_all(fd, program, h.count * sizeof program[0]) != 0) return NULL;
  unsigned char extra;
  if (read(fd, &extra, 1) != 0) return NULL;
  return shim_seccomp_state_new(program, h.count, 1);
}
