#include <fcntl.h>
#include <linux/aio_abi.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

struct aio_sigset { const uint64_t *sigmask; uint64_t sigsetsize; };

static long setup(unsigned nr, aio_context_t *ctx) {
  register long x0 __asm__("x0") = nr, x1 __asm__("x1") = (long)ctx;
  register long x8 __asm__("x8") = 0;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
  return x0;
}
static long submit(aio_context_t ctx, struct iocb **list) {
  register long x0 __asm__("x0") = ctx, x1 __asm__("x1") = 1;
  register long x2 __asm__("x2") = (long)list, x8 __asm__("x8") = 2;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static long destroy(aio_context_t ctx) {
  register long x0 __asm__("x0") = ctx, x8 __asm__("x8") = 1;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
  return x0;
}
static long pget(aio_context_t ctx, long min, long nr, struct io_event *events,
                 struct timespec *timeout, const struct aio_sigset *sig) {
  register long x0 __asm__("x0") = ctx, x1 __asm__("x1") = min;
  register long x2 __asm__("x2") = nr, x3 __asm__("x3") = (long)events;
  register long x4 __asm__("x4") = (long)timeout, x5 __asm__("x5") = (long)sig;
  register long x8 __asm__("x8") = 292;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3),
                   "r"(x4), "r"(x5), "r"(x8) : "memory");
  return x0;
}

int main(void) {
  aio_context_t ctx = 0;
  if (setup(2, &ctx) || !ctx) return 1;
  struct timespec zero = {0, 0};
  struct io_event events[2] = {{0}};
  if (pget(ctx, 0, 1, events, &zero, (void*)1) != -14) return 2;
  struct aio_sigset sig = {0, 123};
  if (pget(ctx, 0, 1, events, &zero, &sig) != 0) return 3;
  uint64_t mask = 0;
  sig.sigmask = &mask;
  if (pget(ctx, 0, 1, events, &zero, &sig) != -22) return 4;
  sig.sigsetsize = 8;
  sig.sigmask = (void*)1;
  if (pget(ctx, 0, 1, events, &zero, &sig) != -14) return 5;
  sig.sigmask = &mask;
  if (pget(ctx, 0, 1, events, &zero, &sig) != -95) return 6;

  int fd = open("/tmp/aio-pgetevents-data", O_CREAT | O_TRUNC | O_RDWR, 0600);
  if (fd < 0) return 7;
  char data[] = "pget";
  struct iocb cb = {0};
  cb.aio_data = 0x292;
  cb.aio_lio_opcode = IOCB_CMD_PWRITE;
  cb.aio_fildes = (uint32_t)fd;
  cb.aio_buf = (uint64_t)(uintptr_t)data;
  cb.aio_nbytes = sizeof data;
  struct iocb *list[] = {&cb};
  if (submit(ctx, list) != 1) return 8;
  if (pget(ctx, 1, 1, events, &zero, &sig) != -95) return 9;
  if (pget(ctx, 1, 1, events, &zero, 0) != 1) return 10;
  if (events[0].data != 0x292 || events[0].res != sizeof data) return 11;
  if (pget(ctx, 0, 1, events, &zero, 0) != 0) return 12;
  close(fd);
  if (destroy(ctx) != 0) return 13;
  puts("GUEST_IO_PGETEVENTS_OK");
  return 0;
}
