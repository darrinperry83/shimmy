/* DEVELOPMENT/VMAPPLE experiment; included by kern_exec.c after its helpers.
 * Never links the userspace shim or rewrites a Linux instruction.
 */
#include <mach/arm/thread_status.h>
#include "rdxnu_elf_plan.h"
extern void pmap_destroy(pmap_t);
#define RD_LINUX_FLAG 0x08U /* Reserved in pinned p_ladvflag; no proc layout change. */
#define RD_LINUX_STACK_TOP 0x600000000ULL
#define RD_LINUX_STACK_SIZE (1024 * 1024)

static bool rd_linux_enabled(void)
{
    int enabled = 0;
    return PE_parse_boot_argn("rdxnu_linux_elf", &enabled, sizeof(enabled)) && enabled == 1;
}

static load_return_t rd_load_elf(struct image_params *imgp, thread_t thread,
    vm_map_t *mapp, load_result_t *result, struct rd_elf_plan *plan)
{
    if (rd_elf_plan(imgp->ip_vdata, PAGE_SIZE, imgp->ip_vattr->va_data_size, plan))
        return LOAD_BADMACHO;
    /* Privileged ELF and spawn attributes need their own policy audit. */
    if (imgp->ip_vattr->va_mode & (S_ISUID | S_ISGID)) return LOAD_PROTECT;
    pmap_t pmap = pmap_create_options(get_task_ledger(get_threadtask(thread)), 0,
                                     PMAP_CREATE_64BIT | PMAP_CREATE_DISABLE_JOP);
    if (!pmap) return LOAD_RESOURCE;
    vm_map_t map = vm_map_create_with_page_shift(pmap, 0, vm_compute_max_offset(TRUE),
                                                14, VM_MAP_CREATE_DEFAULT);
    if (!map) { pmap_destroy(pmap); return LOAD_RESOURCE; }
    imgp->ip_free_map = map;
    vm_map_cs_enforcement_set(map, FALSE); /* Only the opt-in ELF map. */
    unsigned char *buffer = kalloc_data(RD_ELF_PAGE, Z_WAITOK);
    if (!buffer) return LOAD_RESOURCE;
    load_return_t status = LOAD_SUCCESS;
    proc_t p = vfs_context_proc(imgp->ip_vfs_context);
    for (unsigned i = 0; i < plan->count && !status; i++) {
        struct rd_elf_phdr *seg = &plan->loads[i];
        mach_vm_offset_t addr = seg->vaddr & ~(RD_ELF_PAGE - 1);
        mach_vm_size_t size = ((seg->vaddr + seg->memsz + RD_ELF_PAGE - 1)
                              & ~(RD_ELF_PAGE - 1)) - addr;
        if (mach_vm_allocate_kernel(map, &addr, size, VM_MAP_KERNEL_FLAGS_FIXED())) {
            status = LOAD_NOSPACE; break;
        }
        /* Anonymous allocation provides zeroed BSS, including partial pages. */
        for (uint64_t copied = 0; copied < seg->filesz;) {
            int amount = (int)MIN(RD_ELF_PAGE, seg->filesz - copied);
            int resid = 0;
            int error = vn_rdwr(UIO_READ, imgp->ip_vp, (caddr_t)buffer, amount,
                seg->offset + copied, UIO_SYSSPACE, 0,
                vfs_context_ucred(imgp->ip_vfs_context), &resid, p);
            if (error || resid || vm_map_write_user(map, buffer, seg->vaddr + copied, amount)) {
                status = LOAD_IOERROR; break;
            }
            copied += amount;
        }
        vm_prot_t prot = VM_PROT_READ;
        if (seg->flags & 1) prot |= VM_PROT_EXECUTE;
        if (seg->flags & 2) prot |= VM_PROT_WRITE;
        if (!status && (mach_vm_protect(map, addr, size, FALSE, prot) ||
                        mach_vm_protect(map, addr, size, TRUE, prot))) status = LOAD_PROTECT;
    }
    kfree_data(buffer, RD_ELF_PAGE);
    if (status) return status;
    bzero(result, sizeof(*result));
    result->is_64bit_addr = result->is_64bit_data = TRUE;
    result->unixproc = result->validentry = TRUE;
    result->entry_point = plan->entry;
    result->user_stack = RD_LINUX_STACK_TOP;
    result->user_stack_size = RD_LINUX_STACK_SIZE;
    result->user_stack_alloc_size = RD_LINUX_STACK_SIZE + RD_ELF_PAGE;
    result->min_vm_addr = plan->low;
    result->max_vm_addr = plan->high;
    result->ip_platform = PLATFORM_MACOS;
    result->thread_count = 1;
    imgp->ip_free_map = NULL;
    *mapp = map;
    return LOAD_SUCCESS;
}

static int rd_linux_stack(struct image_params *imgp, user_addr_t *sp,
                         struct rd_elf_plan *plan)
{
    if (imgp->ip_argc < 1 || imgp->ip_envc < 0 ||
        imgp->ip_argc + imgp->ip_envc > 128) return E2BIG;
    size_t bytes = imgp->ip_endenvv - imgp->ip_startargv;
    if (bytes > 128 * 1024) return E2BIG;
    user_addr_t strings = (*sp - bytes) & ~15ULL;
    int error = copyout(imgp->ip_startargv, strings, bytes);
    if (error) return error;
    unsigned char random_bytes[16];
    read_random(random_bytes, sizeof(random_bytes));
    user_addr_t random_addr = strings - sizeof(random_bytes);
    error = copyout(random_bytes, random_addr, sizeof(random_bytes));
    if (error) return error;
    uint64_t words[256] = {0};
    size_t n = 0, offset = 0;
    words[n++] = imgp->ip_argc;
    for (int i = 0; i < imgp->ip_argc + imgp->ip_envc; i++) {
        if (i == imgp->ip_argc) words[n++] = 0;
        if (offset >= bytes) return EINVAL;
        size_t len = strnlen(imgp->ip_startargv + offset, bytes - offset);
        if (len == bytes - offset) return EINVAL;
        words[n++] = strings + offset;
        offset += len + 1;
    }
    /* Empty environment still needs both argv and env terminators. */
    if (!imgp->ip_envc) words[n++] = 0;
    words[n++] = 0;
    words[n++] = 3; words[n++] = plan->phdr;
    words[n++] = 4; words[n++] = sizeof(struct rd_elf_phdr);
    words[n++] = 5; words[n++] = plan->phnum;
    words[n++] = 6; words[n++] = RD_ELF_PAGE;
    words[n++] = 7; words[n++] = 0;
    words[n++] = 9; words[n++] = plan->entry;
    kauth_cred_t cred = kauth_cred_get();
    words[n++] = 11; words[n++] = kauth_cred_getruid(cred);
    words[n++] = 12; words[n++] = kauth_cred_getuid(cred);
    words[n++] = 13; words[n++] = kauth_cred_getrgid(cred);
    words[n++] = 14; words[n++] = kauth_cred_getgid(cred);
    words[n++] = 16; words[n++] = 3; /* Conservative FP + ASIMD only. */
    words[n++] = 23; words[n++] = 0; /* No set-id ELF accepted. */
    words[n++] = 25; words[n++] = random_addr;
    words[n++] = 31; words[n++] = strings;
    words[n++] = 0; words[n++] = 0;
    *sp = (random_addr - n * sizeof(uint64_t)) & ~15ULL;
    return copyout(words, *sp, n * sizeof(uint64_t));
}

/* Linux errno numbers differ from Darwin. Keep the first syscall subset explicit. */
static int rd_linux_errno(int error)
{
    switch (error) {
    case 0: return 0;
    case EPERM: return 1; case ENOENT: return 2; case EINTR: return 4;
    case EIO: return 5; case EBADF: return 9; case EAGAIN: return 11;
    case ENOMEM: return 12; case EACCES: return 13; case EFAULT: return 14;
    case EINVAL: return 22; case ENOSPC: return 28; case EPIPE: return 32;
    case EFBIG: return 27; case EISDIR: return 21; case ENXIO: return 6;
    default: return 5;
    }
}

bool rdxnu_linux_dispatch(arm_saved_state_t *state, struct proc *p);
bool rdxnu_linux_dispatch(arm_saved_state_t *state, struct proc *p)
{
    if (!(os_atomic_load(&p->p_ladvflag, relaxed) & RD_LINUX_FLAG) ||
        !is_saved_state64(state) || (get_saved_state_esr(state) & 0xffff)) return false;
    uint64_t nr = get_saved_state_reg(state, 8);
    int64_t value = -38;
    if (nr == 172) value = proc_getpid(p);
    else if (nr >= 174 && nr <= 177) {
        kauth_cred_t cred = kauth_cred_get();
        switch (nr) {
        case 174: value = kauth_cred_getruid(cred); break;
        case 175: value = kauth_cred_getuid(cred); break;
        case 176: value = kauth_cred_getrgid(cred); break;
        default: value = kauth_cred_getgid(cred); break;
        }
    } else if (nr == 66) {
        struct writev_nocancel_args args = {};
        args.fd = (int)get_saved_state_reg(state, 0);
        args.iovp = (user_addr_t)get_saved_state_reg(state, 1);
        args.iovcnt = (int)get_saved_state_reg(state, 2);
        user_ssize_t amount = 0;
        int error = writev_nocancel(p, &args, &amount);
        value = error ? -rd_linux_errno(error) : amount;
    }
    else if (nr == 64) {
        struct write_nocancel_args args = {};
        args.fd = (int)get_saved_state_reg(state, 0);
        args.cbuf = get_saved_state_reg(state, 1);
        args.nbyte = get_saved_state_reg(state, 2);
        user_ssize_t amount = 0;
        int error = args.nbyte > INT64_MAX ? EINVAL : write_nocancel(p, &args, &amount);
        value = error ? -rd_linux_errno(error) : amount;
    } else if (nr == 93 || nr == 94) {
        struct exit_args args = {};
        args.rval = (int)get_saved_state_reg(state, 0);
        int unused = 0;
        exit(p, &args, &unused);
    }
    set_saved_state_reg(state, 0, (uint64_t)value);
    return true;
}
