#ifndef SHIMMY_UFFD_LINUX_ABI_H
#define SHIMMY_UFFD_LINUX_ABI_H

#include "uffd_backend.h"
#include <stddef.h>
#include <stdint.h>

/* Private, Linux-wire-layout structures; not an enabled guest fd or syscall. */
typedef struct { uint64_t start, len; } rd_uffdio_range;
typedef struct { uint64_t api, features, ioctls; } rd_uffdio_api;
typedef struct { rd_uffdio_range range; uint64_t mode, ioctls; } rd_uffdio_register;
typedef struct __attribute__((packed)) {
  uint8_t event, reserved1;
  uint16_t reserved2;
  uint32_t reserved3;
  union {
    struct { uint64_t flags, address; uint32_t ptid; uint32_t pad; } pagefault;
    uint8_t bytes[24];
  } arg;
} rd_uffd_msg;

enum {
  RD_UFFD_API = 0xAA,
  RD_UFFD_EVENT_PAGEFAULT = 0x12,
  RD_UFFD_REGISTER_MODE_MISSING = 1,
  RD_UFFD_PAGEFAULT_FLAG_WRITE = 1
};

/* Validate a requested minimal API. Never fill an ioctl capability mask yet. */
int rd_uffd_check_api(const rd_uffdio_api *request);
/* Only anonymous MISSING mode, and only after an external mapping check. */
int rd_uffd_register_missing(uffd_backend *b, rd_uffdio_register *request,
                             int (*eligible)(uint64_t, uint64_t, void *), void *ctx);
int rd_uffd_unregister_exact(uffd_backend *b, const rd_uffdio_range *range);
/* Convert internal event to Linux read(2) payload; default address is page-aligned. */
int rd_uffd_pack_pagefault(const uffd_backend_event *event, rd_uffd_msg *out);

#endif
