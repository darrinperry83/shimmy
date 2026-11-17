#include "../src/openat2_compat.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
  struct rd_open_how in = { .flags = 0x80000, .mode = 0, .resolve = 0 }, out;
  assert(rd_openat2_decode(&in, sizeof in, &out) == 0);
  assert(out.flags == in.flags && out.mode == 0 && out.resolve == 0);
  assert(rd_openat2_decode(&in, sizeof in - 1, &out) == -22);
  assert(rd_openat2_decode(NULL, sizeof in, &out) == -14);
  unsigned char extended[32] = {0};
  memcpy(extended, &in, sizeof in);
  assert(rd_openat2_decode(extended, sizeof extended, &out) == 0);
  extended[31] = 1;
  assert(rd_openat2_decode(extended, sizeof extended, &out) == -7);
  assert(rd_openat2_decode(extended, 4097, &out) == -7);
  in.resolve = 0x08; // RESOLVE_BENEATH cannot be silently ignored
  assert(rd_openat2_decode(&in, sizeof in, &out) == -95);
  in.resolve = 0;
  in.flags = 0x200000 | 0x80000 | 0x8000; // O_PATH|CLOEXEC|NOFOLLOW
  assert(rd_openat2_decode(&in, sizeof in, &out) == 0);
  in.flags |= 0x200; // O_TRUNC invalid with O_PATH
  assert(rd_openat2_decode(&in, sizeof in, &out) == -22);
  in.flags = 0x41; in.mode = 0600; // O_CREAT|O_WRONLY
  assert(rd_openat2_decode(&in, sizeof in, &out) == 0);
  in.flags = 0; // nonzero mode without O_CREAT
  assert(rd_openat2_decode(&in, sizeof in, &out) == -22);
  in.mode = 0; in.flags = 0x10000; // O_DIRECT unsupported by the host translator
  assert(rd_openat2_decode(&in, sizeof in, &out) == -95);
  in.flags = 1ULL << 50; // unknown Linux flag
  assert(rd_openat2_decode(&in, sizeof in, &out) == -22);
  puts("openat2 validated subset and rejection paths passed");
  return 0;
}
