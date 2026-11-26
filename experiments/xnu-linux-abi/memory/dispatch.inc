/* Linux memory ABI, with XNU's current task map as the sole backing map.
 * Scope: private anonymous/file mappings, optional fixed replacement, protection/unmap.
 * No second page allocator or userspace gmem/JIT machinery is linked here.
 */
#include <sys/mman.h>
static int64_t rd_linux_memory(struct proc *p, arm_saved_state_t *state, uint64_t nr)
{
    uint64_t address = get_saved_state_reg(state,0);
    uint64_t length = get_saved_state_reg(state,1);
    uint64_t prot = get_saved_state_reg(state,2);
    if (nr == 222) {
        uint64_t flags = get_saved_state_reg(state,3);
        uint64_t offset = get_saved_state_reg(state,5);
        if (!length || (prot & ~7ULL)) return -22;
        /* Decode Linux flags explicitly; Darwin MAP_ANON has a different value.
         * Shared maps, NOREPLACE and other unverified flags remain unsupported.
         */
        if ((flags & ~0x32ULL) || !(flags & 2)) return -95;
        if ((flags & 0x10) && (address & (RD_ELF_PAGE - 1))) return -22;
        if (offset & (RD_ELF_PAGE - 1)) return -22;
        if (offset > INT64_MAX) return -22;
        struct mmap_args args = {};
        args.addr = address;
        args.len = length;
        args.prot = (int)prot;
        args.flags = MAP_PRIVATE;
        if (flags & 0x10) args.flags |= MAP_FIXED;
        if (flags & 0x20) args.flags |= MAP_ANON;
        args.fd = (flags & 0x20) ? -1 : (int)get_saved_state_reg(state,4);
        args.pos = (flags & 0x20) ? 0 : (off_t)offset;
        user_addr_t mapped = 0;
        int error = mmap(p,&args,&mapped);
        return error ? -rd_linux_errno(error) : (int64_t)mapped;
    }
    if (address & (RD_ELF_PAGE - 1)) return -22;
    int32_t unused = 0;
    if (nr == 226) {
        if (prot & ~7ULL) return -22;
        struct mprotect_args args = {};
        args.addr = address;
        args.len = length;
        args.prot = (int)prot;
        int error = mprotect(p,&args,&unused);
        return error ? -rd_linux_errno(error) : 0;
    }
    if (!length) return -22;
    struct munmap_args args = {};
    args.addr = address;
    args.len = length;
    int error = munmap(p,&args,&unused);
    return error ? -rd_linux_errno(error) : 0;
}
