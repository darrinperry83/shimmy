// Private preview: guest code is not granted a host clipboard capability.
// A guest-controlled environment variable cannot authorize a Mac pasteboard.
#include <stddef.h>
void gpuwin_clipboard_write(const char *mime, const char *data, size_t len) {
  (void)mime; (void)data; (void)len;
}
int gpuwin_clipboard_read(const char *mime, char *out, size_t cap) {
  (void)mime; (void)out; (void)cap;
  return -1;
}
int gpuwin_clipboard_count(void) { return 0; }
