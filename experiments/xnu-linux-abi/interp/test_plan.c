#include "elf_plan.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static unsigned char bytes[16384],changed[16384];
int main(int argc,char **argv){
 assert(argc==3);FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);uint64_t size=ftell(f);rewind(f);
 size_t n=fread(bytes,1,sizeof bytes,f);fclose(f);struct rd_elf_plan p={0},sentinel;
 assert(!rd_elf_plan(bytes,n,size,&p));assert(!strcmp(p.interpreter,"/lib/ld-linux-aarch64.so.1"));
 assert(p.entry==p.initial_entry && p.entry>=0x40000000ULL);
 struct rd_elf_ehdr *e=(void *)bytes;unsigned interp=99;
 for(unsigned i=0;i<e->phnum;i++)if(((struct rd_elf_phdr *)(void *)(bytes+e->phoff))[i].type==3)interp=i;
 assert(interp<e->phnum);
 for(unsigned test=0;test<7;test++){
  memcpy(changed,bytes,sizeof bytes);struct rd_elf_phdr *ph=(void *)(changed+e->phoff);struct rd_elf_phdr *ip=ph+interp;
  if(test==0)ip->offset=UINT64_MAX;
  if(test==1)ip->filesz=0;
  if(test==2)ip->filesz=257;
  if(test==3)changed[ip->offset]='x';
  if(test==4)changed[ip->offset+ip->filesz-1]='x';
  if(test==5)changed[ip->offset+2]=0;
  if(test==6)ph[interp?0:1]=*ip;
  memset(&p,0xa5,sizeof p);sentinel=p;
  assert(rd_elf_plan(changed,n,size,&p)==2 && !memcmp(&p,&sentinel,sizeof p));
 }
 assert(rd_elf_plan_at(bytes,n,size,&p,0x80000000ULL,0)==2);
 f=fopen(argv[2],"rb");assert(f);fseek(f,0,SEEK_END);size=ftell(f);rewind(f);n=fread(bytes,1,sizeof bytes,f);fclose(f);
 assert(!rd_elf_plan_at(bytes,n,size,&p,0x80000000ULL,0));assert(p.entry>=0x80000000ULL && !p.interpreter[0]);
 puts("INTERP_PLAN_PASS: dynamic program, independent interpreter bias, malformed and nested interpreter controls");
}
