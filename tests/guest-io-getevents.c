#include <fcntl.h>
#include <linux/aio_abi.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static long call2(long nr, long a0, long a1) {
  register long x0 __asm__("x0") = a0, x1 __asm__("x1") = a1;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
  return x0;
}
static long call3(long nr, long a0, long a1, long a2) {
  register long x0 __asm__("x0") = a0, x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static long getevents(long ctx, long min, long nr, void* events, void* timeout) {
  register long x0 __asm__("x0") = ctx, x1 __asm__("x1") = min;
  register long x2 __asm__("x2") = nr, x3 __asm__("x3") = (long)events;
  register long x4 __asm__("x4") = (long)timeout, x8 __asm__("x8") = 4;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8) : "memory");
  return x0;
}
struct ring_header { uint32_t id, nr, head, tail, magic, compat, incompat, header_len; };

int main(void) {
  aio_context_t ctx = 0;
  if (call2(0, 2, (long)&ctx) || !ctx) return 1;
  struct ring_header* ring = (struct ring_header*)(uintptr_t)ctx;
  struct io_event ev[2] = {{0}};
  struct timespec zero = {0, 0};
  struct timespec brief = {0, 1000000L};
  struct timespec bad_time = {0, 1000000000L};
  if (getevents(0, 0, 1, ev, &zero) != -22) return 2;
  if (getevents(ctx, 2, 1, ev, &zero) != -22) return 3;
  if (getevents(ctx, 0, 1, ev, (void*)1) != -14) return 4;
  if (getevents(ctx, 1, 1, ev, &bad_time) != -22) return 5;
  if (getevents(ctx, 1, 1, ev, &zero) != 0) return 6;
  if (getevents(ctx, 1, 1, ev, &brief) != 0) return 23;

  int fd = open("/tmp/aio-getevents-data", O_CREAT | O_TRUNC | O_RDWR, 0600);
  if (fd < 0) return 7;
  char data[] = "done";
  struct iocb cb = {0};
  cb.aio_data = 0xabc;
  cb.aio_lio_opcode = IOCB_CMD_PWRITE;
  cb.aio_fildes = (uint32_t)fd;
  cb.aio_buf = (uint64_t)(uintptr_t)data;
  cb.aio_nbytes = sizeof data;
  struct iocb* list[] = {&cb};
  if (call3(2, ctx, 1, (long)list) != 1) return 8;
  if (ring->tail != 1 || ring->head != 0) return 9;
  if (getevents(ctx, 1, 1, (void*)1, &zero) != -14) return 10;
  if (getevents(ctx, 1, 1, ev, 0) != 1) return 11;
  if (ev[0].data != 0xabc || ev[0].obj != (uint64_t)(uintptr_t)&cb ||
      ev[0].res != sizeof data || ev[0].res2 != 0 || ring->head != 1) return 12;
  if (getevents(ctx, 0, 1, ev, &zero) != 0) return 13;

  // A direct mmap-ring consumer advances head itself. A later syscall must
  // neither redeliver that event nor leave the bounded queue permanently full.
  cb.aio_data = 0xdef;
  if (call3(2, ctx, 1, (long)list) != 1) return 14;
  if (ring->tail != 2) return 15;
  struct io_event* mapped = (struct io_event*)((char*)ring + ring->header_len);
  if (mapped[1].data != 0xdef) return 16;
  ring->head = 2;
  if (getevents(ctx, 0, 1, ev, &zero) != 0) return 17;
  cb.aio_data = 0x123;
  if (call3(2, ctx, 1, (long)list) != 1) return 18;
  if (getevents(ctx, 1, 1, ev, 0) != 1 || ev[0].data != 0x123) return 19;
  if (getevents(ctx, 0, 1, ev, &zero) != 0) return 20;

  if (call2(1, ctx, 0) != 0) return 21;
  if (getevents(ctx, 0, 1, ev, &zero) != -22) return 22;
  close(fd);
  puts("GUEST_IO_GETEVENTS_OK");
  return 0;
}
