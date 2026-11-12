/* rdgpu_surface_handoff.c — publish/import IOSurface mach-port send-rights keyed
 * by a virtual dma-buf carrier fd's (st_dev, st_ino) identity.
 *
 * Export side (shim_cli): mock_fd_export has a completed IOSurface. It calls
 * rdgpu_handoff_publish(), which materializes the send-right with
 * IOSurfaceCreateMachPort(surface) and records it under the carrier identity.
 *
 * Import side (shim_gui): drmasahi.c PRIME_FD_TO_HANDLE receives the SCM_RIGHTS
 * fd, fstat()s it for (st_dev, st_ino), and rdgpu_handoff_resolve_fd() redeems
 * that identity with IOSurfaceLookupFromMachPort(), returning a CFRetained
 * IOSurfaceRef for the gem slot.
 *
 * An AF_UNIX SCM_RIGHTS fd cannot carry a mach port, so the send-right travels
 * on a separate mach channel keyed by (device, inode).
 *
 * TRANSPORT. bootstrap_register() is documented as denied for unprivileged
 * processes, but on this host an unprivileged process CAN register a name in the
 * per-user session bootstrap namespace (verified: kr==BOOTSTRAP_SUCCESS). We use
 * that: the importer (shim_gui, which starts first) allocates a receive right,
 * registers it under a per-run name derived from $RDSOCKETGROUP, and spins a
 * receive thread. The exporter (shim_cli) bootstrap_look_up()s that name and
 * mach_msg()s the IOSurface send-right to the importer keyed by (dev, ino).
 * No launchd plist, no code signing, no parent fd handoff is required. */
#include "rdgpu_surface_handoff.h"
#include <CoreFoundation/CoreFoundation.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <pthread.h>
#include <servers/bootstrap.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define RDGPU_HANDOFF_MAX_SURFACES 64
#define RDGPU_HANDOFF_MAGIC 0x52444846u /* "RDHF" */
#define RDGPU_HANDOFF_VERSION 1u
#define RDGPU_HANDOFF_MSG_MAGIC 0x5244484Du /* "RDHM" */
#define RDGPU_HANDOFF_MSG_ID    0x48464431u /* "HFD1" */

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

typedef struct {
    dev_t device;
    ino_t inode;
    IOSurfaceRef surface;   /* CFRetained: keeps the surface alive while published */
    mach_port_t port;       /* IOSurfaceCreateMachPort send-right */
    int live;
} RDGPUHandoffEntry;

static RDGPUHandoffEntry g_entries[RDGPU_HANDOFF_MAX_SURFACES];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* --- shared file registry (cross-process identity, keyed by dev/ino) ------- */
/* A mach port cannot be persisted to a file. This registry records the carrier
 * identity and surface ID so the importer can audit that a surface was
 * published for the exact fd identity, and so a missed cross-process delivery
 * (e.g. the importer was not yet listening) can still be diagnosed. */

typedef struct {
    uint32_t magic;
    uint16_t version, bytes;
    uint32_t surface_id;
    uint64_t inode;
    int32_t device;
    int32_t reserved;
} RDGPUHandoffRecord;

static void registry_dir(char *out, size_t cap) {
    snprintf(out, cap, "/tmp/rdmetal-handoff-%u", (unsigned)getuid());
}

static void registry_path(char *out, size_t cap, dev_t device, ino_t inode) {
    char dir[256];
    registry_dir(dir, sizeof dir);
    snprintf(out, cap, "%s/%d.%llu", dir, (int)device, (unsigned long long)inode);
}

static void rdgpu_handoff_record(dev_t device, ino_t inode, uint32_t surface_id) {
    char dir[256];
    registry_dir(dir, sizeof dir);
    if (mkdir(dir, 0700) && errno != EEXIST) return;
    char path[512];
    registry_path(path, sizeof path, device, inode);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return;
    RDGPUHandoffRecord r = {
        .magic = RDGPU_HANDOFF_MAGIC,
        .version = RDGPU_HANDOFF_VERSION,
        .bytes = sizeof r,
        .surface_id = surface_id,
        .inode = inode,
        .device = (int32_t)device,
        .reserved = 0,
    };
    ssize_t n = write(fd, &r, sizeof r);
    (void)n;
    close(fd);
}

static void rdgpu_handoff_unrecord(dev_t device, ino_t inode) {
    char path[512];
    registry_path(path, sizeof path, device, inode);
    unlink(path);
}

static uint32_t rdgpu_handoff_registered_surface_id(dev_t device, ino_t inode) {
    char path[512];
    registry_path(path, sizeof path, device, inode);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    RDGPUHandoffRecord r;
    ssize_t n = read(fd, &r, sizeof r);
    close(fd);
    if (n != (ssize_t)sizeof r || r.magic != RDGPU_HANDOFF_MAGIC ||
        r.version != RDGPU_HANDOFF_VERSION || r.bytes != sizeof r ||
        r.device != (int32_t)device || r.inode != inode)
        return 0;
    return r.surface_id;
}

/* --- cross-process mach-port delivery ------------------------------------- */
/* The exporter sends a mach_msg carrying the IOSurface send-right (plus the
 * (dev, ino, surface_id) key) to the importer's receive right. The importer's
 * receive thread stores the right in a process-local table; resolve_fd redeems
 * it with IOSurfaceLookupFromMachPort(). */

typedef struct {
    mach_msg_header_t header;
    mach_msg_body_t body;
    mach_msg_port_descriptor_t port;
    uint32_t magic;
    uint32_t surface_id;
    int32_t device;
    uint32_t reserved;
    uint64_t inode;
} RDGPUHandoffMsg;

typedef struct {
    dev_t device;
    ino_t inode;
    uint32_t surface_id;
    mach_port_t port;       /* received IOSurface send-right (importer's name space) */
    int live;
} RDGPUHandoffImport;

static RDGPUHandoffImport g_imports[RDGPU_HANDOFF_MAX_SURFACES];
static mach_port_t g_recv_port = MACH_PORT_NULL;
static pthread_t g_import_thread;
static volatile sig_atomic_t g_import_stop = 0;
static pthread_once_t g_import_once = PTHREAD_ONCE_INIT;

/* Per-run rendezvous name. Both shims inherit the same $RDSOCKETGROUP (a
 * unique /tmp/mcXXXX directory minted by the common launcher), so they derive
 * the same bootstrap name. Fall back to uid when the variable is absent (unit
 * tests that launch their own processes still share the per-user namespace). */
static void rdgpu_handoff_service_name(char *out, size_t cap) {
    const char *group = getenv("RDSOCKETGROUP");
    const char *base = NULL;
    if (group && *group) {
        base = strrchr(group, '/');
        base = base ? base + 1 : group;
    }
    if (base && *base)
        snprintf(out, cap, "com.rdx.rdgpu.handoff.%s", base);
    else
        snprintf(out, cap, "com.rdx.rdgpu.handoff.u%u", (unsigned)getuid());
}

static void *rdgpu_handoff_receive_thread(void *arg) {
    (void)arg;
    while (!g_import_stop) {
        uint8_t buffer[sizeof(RDGPUHandoffMsg) + 64];
        mach_msg_header_t *hdr = (mach_msg_header_t *)buffer;
        memset(buffer, 0, sizeof buffer);
        hdr->msgh_size = (mach_msg_size_t)sizeof buffer;
        hdr->msgh_local_port = g_recv_port;
        kern_return_t kr = mach_msg(hdr, MACH_RCV_MSG, 0, sizeof buffer,
                                    g_recv_port, 500, MACH_PORT_NULL);
        if (kr == MACH_RCV_TIMED_OUT) continue;
        if (kr != KERN_SUCCESS) { usleep(1000); continue; }
        if (hdr->msgh_id != RDGPU_HANDOFF_MSG_ID) continue;
        if (hdr->msgh_size < (mach_msg_size_t)sizeof(RDGPUHandoffMsg)) continue;
        RDGPUHandoffMsg *msg = (RDGPUHandoffMsg *)buffer;
        if (msg->magic != RDGPU_HANDOFF_MSG_MAGIC) continue;
        if (msg->body.msgh_descriptor_count < 1) continue;
        mach_port_t port = msg->port.name;
        if (port == MACH_PORT_NULL) continue;

        pthread_mutex_lock(&g_lock);
        int slot = -1;
        for (int i = 0; i < RDGPU_HANDOFF_MAX_SURFACES; i++)
            if (g_imports[i].live && g_imports[i].device == (dev_t)msg->device &&
                g_imports[i].inode == (ino_t)msg->inode) { slot = i; break; }
        if (slot < 0)
            for (int i = 0; i < RDGPU_HANDOFF_MAX_SURFACES; i++)
                if (!g_imports[i].live) { slot = i; break; }
        if (slot >= 0) {
            if (g_imports[slot].live)
                mach_port_deallocate(mach_task_self(), g_imports[slot].port);
            g_imports[slot].device = (dev_t)msg->device;
            g_imports[slot].inode = (ino_t)msg->inode;
            g_imports[slot].surface_id = msg->surface_id;
            g_imports[slot].port = port;
            g_imports[slot].live = 1;
        } else {
            mach_port_deallocate(mach_task_self(), port);
        }
        pthread_mutex_unlock(&g_lock);

        fprintf(stderr, "RDGPU_HANDOFF_IMPORT dev=%d ino=%llu id=%u port=%u\n",
                msg->device, (unsigned long long)msg->inode, msg->surface_id, port);
    }
    return NULL;
}

static void rdgpu_handoff_import_init_once(void) {
    kern_return_t kr = mach_port_allocate(mach_task_self(),
                                          MACH_PORT_RIGHT_RECEIVE, &g_recv_port);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "RDGPU_HANDOFF_IMPORT_INIT allocate failed kr=%d\n", kr);
        return;
    }
    /* A send-right for the same receive port is what bootstrap_register hands
     * to any look_up caller (the exporter). */
    kr = mach_port_insert_right(mach_task_self(), g_recv_port, g_recv_port,
                                MACH_MSG_TYPE_MAKE_SEND);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "RDGPU_HANDOFF_IMPORT_INIT insert-right failed kr=%d\n", kr);
        mach_port_deallocate(mach_task_self(), g_recv_port);
        g_recv_port = MACH_PORT_NULL;
        return;
    }
    char name[BOOTSTRAP_MAX_NAME_LEN];
    rdgpu_handoff_service_name(name, sizeof name);
    mach_port_t bp = MACH_PORT_NULL;
    if (task_get_bootstrap_port(mach_task_self(), &bp) != KERN_SUCCESS) {
        fprintf(stderr, "RDGPU_HANDOFF_IMPORT_INIT no bootstrap port\n");
        return;
    }
    kr = bootstrap_register(bp, name, g_recv_port);
    if (kr != BOOTSTRAP_SUCCESS)
        fprintf(stderr, "RDGPU_HANDOFF_IMPORT_INIT bootstrap_register(%s) kr=%d\n",
                name, kr);

    if (pthread_create(&g_import_thread, NULL, rdgpu_handoff_receive_thread, NULL) != 0) {
        fprintf(stderr, "RDGPU_HANDOFF_IMPORT_INIT thread create failed\n");
        return;
    }
    pthread_detach(g_import_thread);
    fprintf(stderr, "RDGPU_HANDOFF_IMPORT_INIT name=%s recv=%u\n", name, g_recv_port);
}

void rdgpu_handoff_import_init(void) {
    pthread_once(&g_import_once, rdgpu_handoff_import_init_once);
}

static IOSurfaceRef rdgpu_handoff_import_lookup(dev_t device, ino_t inode) {
    mach_port_t port = MACH_PORT_NULL;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < RDGPU_HANDOFF_MAX_SURFACES; i++)
        if (g_imports[i].live && g_imports[i].device == device &&
            g_imports[i].inode == inode) {
            port = g_imports[i].port;
            break;
        }
    pthread_mutex_unlock(&g_lock);
    if (port == MACH_PORT_NULL) return NULL;
    return IOSurfaceLookupFromMachPort(port); /* +1 reference */
}

/* Export side: deliver the IOSurface send-right to the importer's receive
 * right over the per-run bootstrap name. Bounded look_up retry tolerates the
 * importer still starting up (the compositor normally registers first). */
static int rdgpu_handoff_deliver(dev_t device, ino_t inode,
                                 uint32_t surface_id, mach_port_t port) {
    char name[BOOTSTRAP_MAX_NAME_LEN];
    rdgpu_handoff_service_name(name, sizeof name);
    mach_port_t bp = MACH_PORT_NULL;
    if (task_get_bootstrap_port(mach_task_self(), &bp) != KERN_SUCCESS) return -ENOTSUP;

    mach_port_t srv = MACH_PORT_NULL;
    for (int attempt = 0; attempt < 100 && srv == MACH_PORT_NULL; attempt++) {
        kern_return_t kr = bootstrap_look_up(bp, name, &srv);
        if (kr != KERN_SUCCESS) {
            if (srv != MACH_PORT_NULL) {
                mach_port_deallocate(mach_task_self(), srv);
                srv = MACH_PORT_NULL;
            }
            usleep(10000);
        }
    }
    if (srv == MACH_PORT_NULL) {
        fprintf(stderr, "RDGPU_HANDOFF_DELIVER no importer at bootstrap name %s\n", name);
        return -ENOTSUP;
    }

    RDGPUHandoffMsg msg;
    memset(&msg, 0, sizeof msg);
    msg.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) |
                           MACH_MSGH_BITS_COMPLEX;
    msg.header.msgh_size = sizeof msg;
    msg.header.msgh_remote_port = srv;
    msg.header.msgh_local_port = MACH_PORT_NULL;
    msg.header.msgh_id = RDGPU_HANDOFF_MSG_ID;
    msg.body.msgh_descriptor_count = 1;
    msg.port.name = port;
    msg.port.disposition = MACH_MSG_TYPE_COPY_SEND;
    msg.port.type = MACH_MSG_PORT_DESCRIPTOR;
    msg.magic = RDGPU_HANDOFF_MSG_MAGIC;
    msg.surface_id = surface_id;
    msg.device = (int32_t)device;
    msg.reserved = 0;
    msg.inode = (uint64_t)inode;

    kern_return_t kr = mach_msg(&msg.header, MACH_SEND_MSG, sizeof msg, 0,
                                MACH_PORT_NULL, 0, MACH_PORT_NULL);
    mach_port_deallocate(mach_task_self(), srv);
    return kr == KERN_SUCCESS ? 0 : -EIO;
}

int rdgpu_handoff_publish(IOSurfaceRef surface, dev_t device, ino_t inode) {
    if (!surface || device == 0 || inode == 0) return -EINVAL;
    mach_port_t port = IOSurfaceCreateMachPort(surface);
    if (port == MACH_PORT_NULL) return -EIO;

    pthread_mutex_lock(&g_lock);
    int slot = -1;
    for (int i = 0; i < RDGPU_HANDOFF_MAX_SURFACES; i++) {
        if (g_entries[i].live && g_entries[i].device == device &&
            g_entries[i].inode == inode) {
            /* Re-publish the same carrier identity: drop the previous right. */
            CFRelease(g_entries[i].surface);
            mach_port_deallocate(mach_task_self(), g_entries[i].port);
            g_entries[i].surface = NULL;
            g_entries[i].port = MACH_PORT_NULL;
            g_entries[i].live = 0;
            slot = i;
            break;
        }
    }
    if (slot < 0)
        for (int i = 0; i < RDGPU_HANDOFF_MAX_SURFACES; i++)
            if (!g_entries[i].live) { slot = i; break; }
    if (slot < 0) {
        pthread_mutex_unlock(&g_lock);
        mach_port_deallocate(mach_task_self(), port);
        return -ENOSPC;
    }
    CFRetain(surface);
    g_entries[slot].device = device;
    g_entries[slot].inode = inode;
    g_entries[slot].surface = surface;
    g_entries[slot].port = port;
    g_entries[slot].live = 1;
    pthread_mutex_unlock(&g_lock);

    /* Cross-process identity registry (audit + failure diagnosis). */
    rdgpu_handoff_record(device, inode, IOSurfaceGetID(surface));

    /* Deliver the send-right to the importing process. */
    int delivered = rdgpu_handoff_deliver(device, inode, IOSurfaceGetID(surface), port);
    fprintf(stderr, "RDGPU_HANDOFF_PUBLISH dev=%d ino=%llu id=%u port=%u "
                    "cross_process_delivery=%d\n",
            (int)device, (unsigned long long)inode, IOSurfaceGetID(surface),
            port, delivered);
    return 0;
}

IOSurfaceRef rdgpu_handoff_resolve_fd(int fd) {
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || st.st_dev == 0 || st.st_ino == 0) return NULL;

    /* In-process publish->resolve (both sides in one shim; unit-tested). */
    pthread_mutex_lock(&g_lock);
    mach_port_t port = MACH_PORT_NULL;
    for (int i = 0; i < RDGPU_HANDOFF_MAX_SURFACES; i++)
        if (g_entries[i].live && g_entries[i].device == st.st_dev &&
            g_entries[i].inode == st.st_ino) {
            port = g_entries[i].port;
            break;
        }
    pthread_mutex_unlock(&g_lock);
    if (port != MACH_PORT_NULL)
        return IOSurfaceLookupFromMachPort(port); /* +1 reference */

    /* Cross-process: the surface lives in shim_cli. Ensure the importer's
     * receive channel is up, then wait (bounded) for the receive thread to
     * record the delivered right. */
    rdgpu_handoff_import_init();
    IOSurfaceRef surface = NULL;
    for (int attempt = 0; attempt < 400 && !surface; attempt++) {
        surface = rdgpu_handoff_import_lookup(st.st_dev, st.st_ino);
        if (!surface) usleep(5000);  /* 400 * 5ms = 2s bound */
    }
    if (surface) {
        fprintf(stderr, "RDGPU_HANDOFF_RESOLVE fd=%d dev=%d ino=%llu resolved id=%u\n",
                fd, (int)st.st_dev, (unsigned long long)st.st_ino,
                IOSurfaceGetID(surface));
        return surface;
    }

    uint32_t sid = rdgpu_handoff_registered_surface_id(st.st_dev, st.st_ino);
    fprintf(stderr, "RDGPU_HANDOFF_RESOLVE fd=%d dev=%d ino=%llu registered_surface_id=%u "
                    "transport_unavailable\n",
            fd, (int)st.st_dev, (unsigned long long)st.st_ino, sid);
    return NULL;
}

void rdgpu_handoff_revoke(dev_t device, ino_t inode) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < RDGPU_HANDOFF_MAX_SURFACES; i++)
        if (g_entries[i].live && g_entries[i].device == device &&
            g_entries[i].inode == inode) {
            CFRelease(g_entries[i].surface);
            mach_port_deallocate(mach_task_self(), g_entries[i].port);
            g_entries[i].surface = NULL;
            g_entries[i].port = MACH_PORT_NULL;
            g_entries[i].live = 0;
            break;
        }
    for (int i = 0; i < RDGPU_HANDOFF_MAX_SURFACES; i++)
        if (g_imports[i].live && g_imports[i].device == device &&
            g_imports[i].inode == inode) {
            mach_port_deallocate(mach_task_self(), g_imports[i].port);
            g_imports[i].port = MACH_PORT_NULL;
            g_imports[i].live = 0;
            break;
        }
    pthread_mutex_unlock(&g_lock);
    rdgpu_handoff_unrecord(device, inode);
}

#pragma clang diagnostic pop
