#include "elf_plan.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static unsigned char image[16384], changed[16384];
static size_t count;
static uint64_t size;
static int check(unsigned char *data, size_t n, uint64_t file_size) {
    struct rd_elf_plan p = {0};
    return rd_elf_plan(data,n,file_size,&p);
}
#define MUTATE(statement) do { memcpy(changed,image,sizeof image); \
    struct rd_elf_ehdr *e=(void *)changed; \
    struct rd_elf_phdr *p=(void *)(changed+e->phoff); \
    while (p->type != 1) p++; \
    (void)e; (void)p; statement; assert(check(changed,count,size)==2); } while(0)
int main(int argc, char **argv) {
    assert(argc==2);
    FILE *f=fopen(argv[1],"rb"); assert(f);
    assert(!fseek(f,0,SEEK_END)); size=(uint64_t)ftell(f); rewind(f);
    count=fread(image,1,sizeof image,f); fclose(f);
    assert(check(image,count,size)==0);
    for(size_t n=0;n<sizeof(struct rd_elf_ehdr);n++) assert(check(image,n,size)!=0);
    MUTATE(e->machine=62);
    MUTATE(e->type=3);
    MUTATE(e->phoff=UINT64_MAX);
    MUTATE(e->phnum=65535);
    MUTATE(e->phentsize=1);
    MUTATE(e->entry=0);
    MUTATE(e->entry|=1);
    MUTATE(p[0].type=3);
    MUTATE(p[0].memsz=0);
    MUTATE(p[0].filesz=p[0].memsz+1);
    MUTATE(p[0].offset=UINT64_MAX);
    MUTATE(p[0].vaddr=UINT64_MAX-10);
    MUTATE(p[0].flags=7);
    MUTATE(p[0].align=3);
    MUTATE(p[1].vaddr=p[0].vaddr);
    MUTATE(p[1].flags=4);
    assert(check(image,count,63)==2);
    /* Deterministic header mutations under ASan/UBSan: never trust a valid image's fields. */
    uint32_t seed=0x12345678;
    for(unsigned i=0;i<20000;i++) {
        memcpy(changed,image,sizeof image);
        seed=seed*1664525U+1013904223U;
        size_t pos=seed % count;
        changed[pos]^=(unsigned char)((seed>>24)|1);
        (void)check(changed, count, size);
    }
    puts("ELF_PLAN_PASS: real fixture, rejection cases, 20000 mutations");
}
