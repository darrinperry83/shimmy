/* AArch64 static PIE: exercise the exported vDSO function and raw #168. */
#include <elf.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/syscall.h>
#include <unistd.h>

typedef long (*getcpu_fn)(uint32_t *, uint32_t *, void *);

static getcpu_fn find_getcpu(void) {
  uintptr_t base = (uintptr_t)getauxval(AT_SYSINFO_EHDR);
  if (!base) return NULL;
  const Elf64_Ehdr *eh = (const Elf64_Ehdr *)base;
  if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_machine != EM_AARCH64)
    return NULL;
  const Elf64_Phdr *ph = (const Elf64_Phdr *)(base + eh->e_phoff);
  const Elf64_Dyn *dyn = NULL;
  for (unsigned i = 0; i < eh->e_phnum; ++i)
    if (ph[i].p_type == PT_DYNAMIC) dyn = (const Elf64_Dyn *)(base + ph[i].p_vaddr);
  if (!dyn) return NULL;
  const char *strings = NULL;
  const Elf64_Sym *symbols = NULL;
  const uint32_t *hash = NULL;
  for (; dyn->d_tag != DT_NULL; ++dyn) {
    if (dyn->d_tag == DT_STRTAB) strings = (const char *)(base + dyn->d_un.d_ptr);
    if (dyn->d_tag == DT_SYMTAB) symbols = (const Elf64_Sym *)(base + dyn->d_un.d_ptr);
    if (dyn->d_tag == DT_HASH) hash = (const uint32_t *)(base + dyn->d_un.d_ptr);
  }
  if (!strings || !symbols || !hash || hash[1] > 64) return NULL;
  for (uint32_t i = 0; i < hash[1]; ++i)
    if (strcmp(strings + symbols[i].st_name, "__kernel_getcpu") == 0)
      return (getcpu_fn)(base + symbols[i].st_value);
  return NULL;
}

int main(void) {
  getcpu_fn vdso = find_getcpu();
  if (!vdso) return 1;
  uint64_t mask = 0;
  if (syscall(SYS_sched_getaffinity, 0, sizeof mask, &mask) != 8) return 2;
  if (vdso((uint32_t *)1, NULL, NULL) != -EFAULT) return 3;
  if (vdso(NULL, (uint32_t *)1, NULL) != -EFAULT) return 4;
  if (vdso(NULL, NULL, (void *)1) != 0) return 5; /* cache ignored */
  uint32_t node = 99;
  if (vdso(NULL, &node, NULL) != 0 || node != 0) return 6;
  int observed_nonzero = 0;
  for (int i = 0; i < 2000; ++i) {
    unsigned raw = 99;
    uint32_t cpu = 99;
    if (syscall(SYS_getcpu, &raw, NULL, NULL) != 0) return 7;
    if (vdso(&cpu, &node, NULL) != 0) return 8;
    if (raw >= 32 || !(mask & (UINT64_C(1) << raw))) return 9;
    if (cpu >= 32 || !(mask & (UINT64_C(1) << cpu)) || node != 0) return 10;
    if (cpu != 0) observed_nonzero = 1;
  }
  if (!observed_nonzero) return 11; /* catches the old constant-zero vDSO */
  puts("GETCPU_VDSO_PASS");
  return 0;
}
