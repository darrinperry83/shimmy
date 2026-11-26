/* Mirror only bytes accepted by stdout/stderr. Copyin failures must not change
 * the original syscall result. Bounded output is diagnostic, not tty semantics.
 * User memory can change after the write; this is not an atomic tee. */
extern void rdxnu_console_begin(void);
extern void rdxnu_console_output(const char *, size_t);
static void rd_console_mirror(int fd, user_addr_t address, uint64_t length)
{
    if (fd != 1 && fd != 2) return;
    char buffer[256];
    length = MIN(length, 65536ULL);
    while (length) {
        size_t chunk = (size_t)MIN(length, sizeof(buffer));
        if (address > UINT64_MAX - chunk || copyin(address, buffer, chunk)) break;
        rdxnu_console_output(buffer, chunk);
        address += chunk;
        length -= chunk;
    }
}
static void rd_console_mirror_iov(int fd, user_addr_t address, int count, uint64_t length)
{
    if ((fd != 1 && fd != 2) || count < 0 || count > 1024) return;
    length = MIN(length, 65536ULL);
    for (int i = 0; i < count && length; ++i) {
        struct { uint64_t base, length; } vector;
        if (address > UINT64_MAX - sizeof(vector) || copyin(address, &vector, sizeof(vector))) break;
        uint64_t chunk = MIN(length, vector.length);
        rd_console_mirror(fd, vector.base, chunk);
        length -= chunk;
        address += sizeof(vector);
    }
}
