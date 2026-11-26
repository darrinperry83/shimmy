#include "elf_plan.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static unsigned char bytes[16384], changed[16384];
int main(int argc,char **argv) {
    assert(argc==3);
    FILE *f=fopen(argv[1],"rb"); assert(f);
    fseek(f,0,SEEK_END); uint64_t size=(uint64_t)ftell(f); rewind(f);
    size_t n=fread(bytes,1,sizeof bytes,f); fclose(f);
    struct rd_elf_plan p={0};
    assert(rd_elf_plan(bytes,n,size,&p)==0);
    assert(p.entry>=0x40000000ULL && p.phdr>=0x40000000ULL);
    memcpy(changed,bytes,sizeof bytes);
    struct rd_elf_ehdr *e=(void *)changed;
    struct rd_elf_phdr *ph=(void *)(changed+e->phoff);
    ph->type=3; // A PT_INTERP still requires a separate loader milestone.
    assert(rd_elf_plan(changed,n,size,&p)==2);
    uint32_t seed=123;
    for(unsigned i=0;i<20000;i++) {
        memcpy(changed,bytes,sizeof bytes);
        seed=seed*1664525U+1013904223U;
        changed[seed%n]^=(unsigned char)((seed>>24)|1);
        (void)rd_elf_plan(changed,n,size,&p);
    }
    f=fopen(argv[2],"rb"); assert(f);
    fseek(f,0,SEEK_END); size=(uint64_t)ftell(f); rewind(f);
    n=fread(bytes,1,sizeof bytes,f); fclose(f);
    assert(rd_elf_plan(bytes,n,size,&p)==2);
    puts("ARCH_LOADER_PLAN_PASS: unmodified ld.so accepted, PT_INTERP rejected, mutations checked");
}
