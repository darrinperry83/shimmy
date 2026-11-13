// Does macOS clobber TPIDR_EL0 across context switches on a worker pthread?
// Set it in user mode, force preemption, re-read. If it changes, the shim's
// per-trap TPIDR restore is fundamentally insufficient.
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <sched.h>
#include <stdint.h>

static inline uint64_t rd_tpidr(void){ uint64_t v; __asm__ volatile("mrs %0, tpidr_el0":"=r"(v)); return v; }
static inline void     wr_tpidr(uint64_t v){ __asm__ volatile("msr tpidr_el0, %0"::"r"(v)); }

static void* worker(void* _){
  const uint64_t MAGIC = 0x10e1dede8ULL;   // same shape as the guest's musl TP
  wr_tpidr(MAGIC);
  uint64_t after_set = rd_tpidr();
  printf("[worker] set=%#llx  immediately-read=%#llx  %s\n",
         MAGIC, after_set, after_set==MAGIC ? "OK" : "LOST-IMMEDIATELY");

  int clobbered=0; uint64_t seen=MAGIC;
  for (int i=0;i<2000000 && !clobbered;i++){
    if ((i & 0x3fff)==0) { sched_yield(); usleep(200); }   // encourage preemption
    uint64_t v = rd_tpidr();
    if (v != MAGIC){ clobbered=1; seen=v; printf("[worker] CLOBBERED after %d reads: tpidr=%#llx\n",i,v); }
  }
  if (!clobbered) printf("[worker] survived 2e6 reads + yields: tpidr stayed %#llx\n", rd_tpidr());
  (void)seen;
  return 0;
}

int main(void){
  printf("[main] tpidr at start=%#llx\n", rd_tpidr());
  pthread_t t; pthread_create(&t,0,worker,0); pthread_join(t,0);
  return 0;
}
