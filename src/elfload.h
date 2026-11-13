// elfload.h — aarch64 Linux ELF loader (PIE + dynamic), mapping through gmem.
#ifndef RDRIFT_ELFLOAD_H
#define RDRIFT_ELFLOAD_H
#ifdef __cplusplus
extern "C" {
#endif

struct loaded {
  unsigned long base;        // load bias actually used
  unsigned long entry;       // absolute entry point (base + e_entry)
  unsigned long phdr;        // absolute address of the program headers in guest memory
  int           phnum;
  int           phent;
  char          interp[256]; // PT_INTERP path, "" if static
};

// Load an ELF image from a HOST path. Maps PT_LOAD segments via gmem (exec -> JIT, data -> RW)
// and applies gmem_rewrite_exec_insns() to every executable segment before it lands in JIT.
// Returns 0 on success, non-zero on failure.
int elf_load(const char* host_path, struct loaded* out);

#ifdef __cplusplus
}
#endif
#endif
