static bool rd_linux_enabled(void)
{
    int enabled = 0;
    return PE_parse_boot_argn("rdxnu_linux_elf", &enabled, sizeof(enabled)) && enabled == 1;
}

static load_return_t rd_elf_map_segments(struct image_params *imgp, vnode_t vp,
    vm_map_t map, struct rd_elf_plan *plan, unsigned char *buffer)
{
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
            int error = vn_rdwr(UIO_READ, vp, (caddr_t)buffer, amount,
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
    return status;
}

#include "rdxnu_interp_image.h"

static load_return_t rd_load_elf(struct image_params *imgp, thread_t thread,
    vm_map_t *mapp, load_result_t *result, struct rd_elf_plan *plan, struct rd_interp_image *interp)
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
    load_return_t status = rd_elf_map_segments(imgp,imgp->ip_vp,map,plan,buffer);
    if (!status && interp) {
        status = rd_elf_map_segments(imgp,interp->vp,map,&interp->plan,buffer);
        plan->interpreter_base = 0x80000000ULL;
        plan->initial_entry = interp->plan.entry;
    }
    kfree_data(buffer, RD_ELF_PAGE);
    if (status) return status;
    bzero(result, sizeof(*result));
    result->is_64bit_addr = result->is_64bit_data = TRUE;
    result->unixproc = result->validentry = TRUE;
    result->entry_point = plan->initial_entry;
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
    const char *exec_path = imgp->ip_strings + strlen(EXECUTABLE_KEY);
    size_t exec_length = strnlen(exec_path,MAXPATHLEN);
    if (exec_length == MAXPATHLEN) return ENAMETOOLONG;
    user_addr_t exec_name = strings - exec_length - 1;
    error = copyout(exec_path,exec_name,exec_length+1);
    if (error) return error;
    unsigned char random_bytes[16];
    read_random(random_bytes, sizeof(random_bytes));
    user_addr_t random_addr = exec_name - sizeof(random_bytes);
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
    words[n++] = 7; words[n++] = plan->interpreter_base;
    words[n++] = 9; words[n++] = plan->entry;
    kauth_cred_t cred = kauth_cred_get();
    words[n++] = 11; words[n++] = kauth_cred_getruid(cred);
    words[n++] = 12; words[n++] = kauth_cred_getuid(cred);
    words[n++] = 13; words[n++] = kauth_cred_getrgid(cred);
    words[n++] = 14; words[n++] = kauth_cred_getgid(cred);
    words[n++] = 16; words[n++] = 3; /* Conservative FP + ASIMD only. */
    words[n++] = 23; words[n++] = 0; /* No set-id ELF accepted. */
    words[n++] = 25; words[n++] = random_addr;
    words[n++] = 31; words[n++] = exec_name;
    words[n++] = 0; words[n++] = 0;
    *sp = (random_addr - n * sizeof(uint64_t)) & ~15ULL;
    return copyout(words, *sp, n * sizeof(uint64_t));
}

