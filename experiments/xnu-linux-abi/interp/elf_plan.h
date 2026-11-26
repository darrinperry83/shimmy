/* Bounded ELF64/AArch64 ET_EXEC planner. Shared by kernel loader and host tests.
 * Format definitions follow the existing src/elfload.c; no gmem/JIT/rewrite path.
 */
#ifndef RDXNU_INTERP_PLAN_H
#define RDXNU_INTERP_PLAN_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define RD_ELF_PAGE 16384ULL
#define RD_ELF_MAX_LOADS 16
#define RD_ELF_MAX_SPAN (16ULL * 1024 * 1024)
struct rd_elf_ehdr {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct rd_elf_phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};
struct rd_elf_plan {
    uint64_t entry, phdr, phnum, low, high;
    uint64_t interpreter_base, initial_entry;
    char interpreter[256];
    unsigned count;
    struct rd_elf_phdr loads[RD_ELF_MAX_LOADS];
};
/* 0 success; 1 unrecognized; 2 malformed/unsupported. No partial plan on error. */
static int rd_elf_plan_at(const void *header, size_t available, uint64_t file_size,
                      struct rd_elf_plan *out, uint64_t dynamic_bias, int allow_interpreter)
{
    struct rd_elf_plan plan = {0};
    struct rd_elf_ehdr e;
    const unsigned char *bytes = header;
    if (available < 4 || memcmp(bytes, "\177ELF", 4)) return 1;
    if (available < sizeof(e)) return 2;
    memcpy(&e, bytes, sizeof(e));
    if (e.ident[4] != 2 || e.ident[5] != 1 || e.ident[6] != 1 ||
        (e.ident[7] != 0 && e.ident[7] != 3) || e.ident[8] != 0 ||
        (e.type != 2 && e.type != 3) || e.machine != 183 || e.version != 1 || e.flags != 0 ||
        e.ehsize != sizeof(e) || e.phentsize != sizeof(struct rd_elf_phdr) ||
        !e.phnum || e.phnum > 32 || e.phoff < sizeof(e)) return 2;
    uint64_t table_size = (uint64_t)e.phnum * e.phentsize;
    if (e.phoff > available || table_size > available - e.phoff ||
        e.phoff > file_size || table_size > file_size - e.phoff) return 2;
    uint64_t bias = e.type == 3 ? dynamic_bias : 0;
    if (e.entry > UINT64_MAX - bias) return 2;
    plan.entry = e.entry + bias;
    plan.phnum = e.phnum;
    plan.low = UINT64_MAX;
    int valid_entry = 0, has_tls = 0;
    for (unsigned i = 0; i < e.phnum; i++) {
        struct rd_elf_phdr p;
        memcpy(&p, bytes + e.phoff + i * e.phentsize, sizeof(p));
        if (p.type == 3) {
            if (!allow_interpreter || plan.interpreter[0] || p.filesz < 2 ||
                p.filesz > sizeof(plan.interpreter) || p.offset > available ||
                p.filesz > available - p.offset || p.offset > file_size ||
                p.filesz > file_size - p.offset) return 2;
            const char *path = (const char *)bytes + p.offset;
            if (path[0] != '/' || path[p.filesz - 1] ||
                strnlen(path, p.filesz) != p.filesz - 1) return 2;
            memcpy(plan.interpreter, path, p.filesz);
            continue;
        }
        /* The dynamic linker owns TLS allocation; reject unbounded metadata. */
        if (p.type == 7) {
            if (has_tls || p.filesz > p.memsz || p.offset > file_size ||
                p.filesz > file_size - p.offset || p.memsz > RD_ELF_MAX_SPAN ||
                (p.align > 1 && (p.align & (p.align - 1)))) return 2;
            has_tls = 1; continue;
        }
        if (p.type == 0x6474e551 && (p.flags & 1)) return 2;
        if (p.type != 1) {
            if (p.type != 0 && p.type != 2 && p.type != 4 && p.type != 6 &&
                p.type != 0x6474e550 && p.type != 0x6474e554 &&
                p.type != 0x6474e551 && p.type != 0x6474e552 &&
                p.type != 0x6474e553) return 2;
            continue;
        }
        if (p.vaddr > UINT64_MAX - bias) return 2;
        p.vaddr += bias;
        if (!p.memsz || p.filesz > p.memsz || p.offset > file_size ||
            p.filesz > file_size - p.offset || p.vaddr < RD_ELF_PAGE ||
            p.vaddr >= 0x100000000ULL || p.memsz > 0x100000000ULL - p.vaddr ||
            p.memsz > RD_ELF_MAX_SPAN || (p.flags & ~7U) ||
            !(p.flags & 4) || (p.flags & 3) == 3 ||
            (p.align > 1 && ((p.align & (p.align - 1)) ||
              ((p.vaddr - p.offset) & (p.align - 1)))) ||
            ((p.vaddr - p.offset) & (RD_ELF_PAGE - 1)) ||
            plan.count == RD_ELF_MAX_LOADS) return 2;
        uint64_t low = p.vaddr & ~(RD_ELF_PAGE - 1);
        uint64_t high = (p.vaddr + p.memsz + RD_ELF_PAGE - 1) & ~(RD_ELF_PAGE - 1);
        for (unsigned j = 0; j < plan.count; j++) {
            uint64_t a = plan.loads[j].vaddr & ~(RD_ELF_PAGE - 1);
            uint64_t z = (plan.loads[j].vaddr + plan.loads[j].memsz + RD_ELF_PAGE - 1)
                         & ~(RD_ELF_PAGE - 1);
            if (low < z && high > a) return 2;
        }
        if (low < plan.low) plan.low = low;
        if (high > plan.high) plan.high = high;
        if ((p.flags & 1) && plan.entry >= p.vaddr && plan.entry - p.vaddr < p.filesz &&
            !(plan.entry & 3)) valid_entry = 1;
        if (p.offset <= e.phoff && table_size <= p.filesz &&
            e.phoff - p.offset <= p.filesz - table_size)
            plan.phdr = p.vaddr + e.phoff - p.offset;
        plan.loads[plan.count++] = p;
    }
    if ((has_tls && !plan.interpreter[0]) || !plan.count || !valid_entry || !plan.phdr ||
        plan.high - plan.low > RD_ELF_MAX_SPAN) return 2;
    plan.initial_entry = plan.entry;
    *out = plan;
    return 0;
}
static int rd_elf_plan(const void *header, size_t available, uint64_t file_size,
                       struct rd_elf_plan *out)
{
    return rd_elf_plan_at(header, available, file_size, out, 0x40000000ULL, 1);
}
#endif
