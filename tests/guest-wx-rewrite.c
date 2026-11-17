/* A Linux JIT page must retain code across RX -> write -> RX flips. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void) {
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    uint32_t *code = mmap(NULL, page, PROT_READ | PROT_WRITE | PROT_EXEC,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) { perror("mmap RWX"); return 1; }
    code[0] = 0x528000e0u; /* mov w0, #7 */
    code[1] = 0xd65f03c0u; /* ret */
    __builtin___clear_cache((char *)code, (char *)(code + 2));
    int (*run)(void) = (int (*)(void))code;
    int first = run();
    code[0] = 0x52800120u; /* mov w0, #9: write fault on RX page */
    __builtin___clear_cache((char *)code, (char *)(code + 2));
    int second = run();
    uint32_t tail = code[1];
    printf("WX_REWRITE first=%d second=%d ret=%08x\n", first, second, tail);
    munmap(code, page);
    return first == 7 && second == 9 && tail == 0xd65f03c0u ? 0 : 2;
}
