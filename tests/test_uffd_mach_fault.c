#define _DARWIN_C_SOURCE
#include "uffd_backend.h"
#include "uffd_linux_abi.h"

#include <assert.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

extern boolean_t mach_exc_server(mach_msg_header_t *, mach_msg_header_t *);
static mach_port_t port;
static uffd_backend *backend;
static volatile unsigned char *page;
static int worker_ok;
static int freshly_reserved(uint64_t start, uint64_t length, void *context) {
  // The test owns this newly created PROT_NONE mapping and has not exposed it
  // to a worker. This guarantee does not exist for arbitrary gmem_mmap calls.
  return start == (uint64_t)(uintptr_t)context && length == 16384;
}

kern_return_t catch_mach_exception_raise(mach_port_t a, mach_port_t b,
    mach_port_t c, exception_type_t d, mach_exception_data_t e,
    mach_msg_type_number_t f) {
  (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
  return KERN_FAILURE;
}
kern_return_t catch_mach_exception_raise_state(mach_port_t a, exception_type_t b,
    mach_exception_data_t c, mach_msg_type_number_t d, int *e, thread_state_t f,
    mach_msg_type_number_t g, thread_state_t h, mach_msg_type_number_t *i) {
  (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i;
  return KERN_FAILURE;
}
kern_return_t catch_mach_exception_raise_state_identity(mach_port_t p,
    mach_port_t th, mach_port_t task, exception_type_t exc,
    mach_exception_data_t code, mach_msg_type_number_t cc, int *flavor,
    thread_state_t old_state, mach_msg_type_number_t old_count,
    thread_state_t new_state, mach_msg_type_number_t *new_count) {
  (void)p; (void)th; (void)task; (void)flavor;
  if (exc != EXC_BAD_ACCESS || cc < 2) return KERN_FAILURE;
  uint64_t address = (uint64_t)code[1];
  if (address < (uint64_t)(uintptr_t)page ||
      address >= (uint64_t)(uintptr_t)page + 16384) return KERN_FAILURE;
  memcpy(new_state, old_state, old_count * sizeof(natural_t));
  *new_count = old_count;
  return uffd_backend_report_fault(backend, address, 0) == 1 ? KERN_SUCCESS : KERN_FAILURE;
}

static void *serve(void *unused) {
  (void)unused;
  mach_msg_server(mach_exc_server, 4096, port, 0);
  return NULL;
}
static void *faulting_thread(void *unused) {
  (void)unused;
  mach_port_t th = mach_thread_self();
  kern_return_t kr = thread_set_exception_ports(th, EXC_MASK_BAD_ACCESS, port,
      EXCEPTION_STATE_IDENTITY | MACH_EXCEPTION_CODES, ARM_THREAD_STATE64);
  mach_port_deallocate(mach_task_self(), th);
  if (kr != KERN_SUCCESS) return NULL;
  unsigned char got = page[0]; // real Darwin protection fault
  worker_ok = (got == 'Q');
  page[0] = 'R';
  return NULL;
}

int main(void) {
  page = mmap(NULL, 16384, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
  assert(page != MAP_FAILED);
  backend = uffd_backend_create();
  assert(backend);
  rd_uffdio_register request = {
    .range = {(uint64_t)(uintptr_t)page, 16384},
    .mode = RD_UFFD_REGISTER_MODE_MISSING
  };
  assert(rd_uffd_register_missing(backend, &request, freshly_reserved, (void *)page) == 0);
  assert(mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port) == KERN_SUCCESS);
  assert(mach_port_insert_right(mach_task_self(), port, port,
                                MACH_MSG_TYPE_MAKE_SEND) == KERN_SUCCESS);
  pthread_t server, worker;
  assert(pthread_create(&server, NULL, serve, NULL) == 0);
  assert(pthread_create(&worker, NULL, faulting_thread, NULL) == 0);

  struct pollfd pfd = {.fd = uffd_backend_poll_fd(backend), .events = POLLIN};
  assert(poll(&pfd, 1, 3000) == 1 && (pfd.revents & POLLIN));
  uffd_backend_event event;
  assert(uffd_backend_read_event(backend, &event) == 1);
  assert(event.address == (uint64_t)(uintptr_t)page);
  rd_uffd_msg linux_event;
  assert(rd_uffd_pack_pagefault(&event, &linux_event) == 0);
  assert(linux_event.event == RD_UFFD_EVENT_PAGEFAULT);
  assert(linux_event.arg.pagefault.address == (uint64_t)(uintptr_t)page);
  assert(mprotect((void *)page, 16384, PROT_READ | PROT_WRITE) == 0);
  page[0] = 'Q';
  assert(uffd_backend_complete(backend, event.sequence) == 0);
  assert(pthread_join(worker, NULL) == 0 && worker_ok && page[0] == 'R');
  uffd_backend_destroy(backend);
  assert(munmap((void *)page, 16384) == 0);
  assert(mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1) == KERN_SUCCESS);
  assert(pthread_join(server, NULL) == 0);
  assert(mach_port_deallocate(mach_task_self(), port) == KERN_SUCCESS);
  puts("UFFD_MACH_FAULT_RELAY_OK");
  return 0;
}
