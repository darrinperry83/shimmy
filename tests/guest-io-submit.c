#include <fcntl.h>
#include <linux/aio_abi.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static long raw2(long nr, long a0, long a1) {
  register long x0 __asm__("x0") = a0;
  register long x1 __asm__("x1") = a1;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
  return x0;
}
static long raw3(long nr, long a0, long a1, long a2) {
  register long x0 __asm__("x0") = a0;
  register long x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
struct ring_header { uint32_t id, nr, head, tail, magic, compat, incompat, header_len; };

int main(void) {
  aio_context_t ctx = 0;
  if (raw2(0, 4, (long)&ctx) != 0 || !ctx) return 1;
  struct ring_header* ring = (struct ring_header*)(uintptr_t)ctx;
  if (ring->nr != 5 || ring->tail != 0) return 2;
  if (raw3(2, ctx, -1, 0) != -22) return 3;
  if (raw3(2, ctx, 0, 0) != 0) return 4;
  int fd = open("/tmp/aio-submit-data", O_CREAT | O_TRUNC | O_RDWR, 0600);
  if (fd < 0) return 5;
  char payload[] = "AIO works";
  struct iocb cb = {0};
  cb.aio_data = 0x1234;
  cb.aio_key = 9;
  cb.aio_lio_opcode = IOCB_CMD_PWRITE;
  cb.aio_fildes = (uint32_t)fd;
  cb.aio_buf = (uint64_t)(uintptr_t)payload;
  cb.aio_nbytes = sizeof payload;
  cb.aio_offset = 2;
  struct iocb* list[] = {&cb};
  if (raw3(2, ctx, 1, (long)list) != 1 || cb.aio_key != 0) return 6;
  if (ring->tail != 1) return 7;
  struct io_event* ev = (struct io_event*)((char*)ring + ring->header_len);
  if (ev[0].data != 0x1234 || ev[0].obj != (uint64_t)(uintptr_t)&cb ||
      ev[0].res != sizeof payload || ev[0].res2 != 0) return 8;
  char check[32] = {0};
  if (pread(fd, check, sizeof payload, 2) != sizeof payload ||
      memcmp(check, payload, sizeof payload)) return 9;

  char readback[32] = {0};
  cb.aio_data = 0x5678;
  cb.aio_lio_opcode = IOCB_CMD_PREAD;
  cb.aio_buf = (uint64_t)(uintptr_t)readback;
  if (raw3(2, ctx, 1, (long)list) != 1 || ring->tail != 2) return 10;
  if (memcmp(readback, payload, sizeof payload) || ev[1].res != sizeof payload) return 11;
  cb.aio_fildes = (uint32_t)-1;
  if (raw3(2, ctx, 1, (long)list) != -9) return 12;
  cb.aio_fildes = (uint32_t)fd;
  cb.aio_lio_opcode = IOCB_CMD_POLL;
  if (raw3(2, ctx, 1, (long)list) != -95) return 13;
  if (raw3(2, ctx, 1, 1) != -14) return 14;
  cb.aio_lio_opcode = IOCB_CMD_PREAD;
  struct iocb* batch[] = {&cb, &cb, &cb};
  if (raw3(2, ctx, 3, (long)batch) != 2 || ring->tail != 4) return 16;
  if (raw3(2, ctx, 1, (long)list) != -11) return 17;
  if (raw2(1, ctx, 0) != 0) return 15;
  close(fd);
  puts("GUEST_IO_SUBMIT_OK");
  return 0;
}
