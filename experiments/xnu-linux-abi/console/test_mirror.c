/* Exercise the production mirror's untrusted pointer/length boundaries. */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define MIN(a,b) ((a) < (b) ? (a) : (b))
typedef uint64_t user_addr_t;
static const void *ranges[2];
static size_t sizes[2];
static char output[131072];
static size_t used, copies;
static int copyin(user_addr_t address, void *buffer, size_t length)
{
    ++copies;
    for (int i = 0; i < 2; ++i) {
        uintptr_t base = (uintptr_t)ranges[i];
        if (address >= base && address - base <= sizes[i] &&
            length <= sizes[i] - (address - base)) {
            memcpy(buffer, (void *)(uintptr_t)address, length);
            return 0;
        }
    }
    return 14;
}
void rdxnu_console_output(const char *buffer, size_t length)
{
    assert(used + length <= sizeof(output));
    memcpy(output + used, buffer, length);
    used += length;
}
#include "mirror.inc"
int main(void)
{
    static char bytes[70000];
    memset(bytes, 'x', sizeof(bytes));
    ranges[0] = bytes; sizes[0] = sizeof(bytes);
    uint64_t addr = (uintptr_t)bytes;
    rd_console_mirror(4, addr, sizeof(bytes));
    assert(used == 0 && copies == 0); /* protocol pipe must remain untouched */
    rd_console_mirror(1, addr, 0);
    assert(used == 0 && copies == 0);
    rd_console_mirror(1, UINT64_MAX - 4, 20);
    assert(used == 0 && copies == 0); /* no pointer wraparound */
    rd_console_mirror(1, 1, 20);
    assert(used == 0);
    rd_console_mirror(1, addr, UINT64_MAX);
    assert(used == 65536); /* bounded even when caller length is hostile */
    used = copies = 0;
    struct { uint64_t base, length; } vectors[] = {{addr, 2}, {addr+2, 5}};
    ranges[1] = vectors; sizes[1] = sizeof(vectors);
    rd_console_mirror_iov(2, (uintptr_t)vectors, 2, 4);
    assert(used == 4); /* partial successful writev must not mirror extra bytes */
    used = copies = 0;
    rd_console_mirror_iov(1, (uintptr_t)vectors, 1025, 4);
    rd_console_mirror_iov(1, (uintptr_t)vectors, -1, 4);
    rd_console_mirror_iov(1, UINT64_MAX - 4, 1, 4);
    assert(used == 0 && copies == 0);
    puts("CONSOLE_MIRROR_BOUNDS_PASS");
}
