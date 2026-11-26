/* Diagnostic XNU host for the pinned Linux/LKL core. No ELF dispatch here.
 * Only the opt-in, one-shot kernel test may enter this core. XNU owns every
 * physical thread, timer and allocation. Linux owns the tested fd/VFS state.
 */
#include <mach/mach_types.h>
#include <mach/semaphore.h>
#include <mach/sync_policy.h>
#include <mach/task.h>
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <kern/misc_protos.h>
#include <string.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include "rdxnu_lkl_api.h"
extern int vsnprintf(char *, size_t, const char *, va_list) __printflike(3, 0);

extern long rdl_lkl_syscall(long, long *);
extern long rdl_lkl_sys_halt(void);
extern void rdxnu_linux_jump_set(struct lkl_jmp_buf *, void (*)(void));
extern void rdxnu_linux_jump_back(struct lkl_jmp_buf *, int);
static LCK_GRP_DECLARE(rd_group, "rdxnu-linux-host");
static LCK_MTX_DECLARE(rd_registry, &rd_group);
static int rd_status;
static const char *rd_stage = "not started";
static LCK_MTX_DECLARE(rd_log_lock, &rd_group);
static char rd_log[4096];
static size_t rd_log_size;
static unsigned rd_trace_count;
static bool rd_tracing;
static void rd_record(const char *text, size_t length)
{
    lck_mtx_lock(&rd_log_lock);
    size_t available = sizeof(rd_log) - 1 - rd_log_size;
    if (length > available) length = available;
    memcpy(rd_log + rd_log_size, text, length); rd_log_size += length;
    rd_log[rd_log_size] = 0; lck_mtx_unlock(&rd_log_lock);
}
static void rd_pause(unsigned ms)
{
    uint64_t deadline; clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
    assert_wait_deadline((event_t)&deadline, THREAD_UNINT, deadline);
    thread_block(THREAD_CONTINUE_NULL);
}
static void rd_trace(const char *operation)
{
    if (!rd_tracing) return;
    unsigned count = __atomic_fetch_add(&rd_trace_count, 1, __ATOMIC_RELAXED);
    if (count >= 80) return;
    char line[128]; int length = snprintf(line,sizeof(line),"HOST %u %s stage=%s\n",count,operation,rd_stage);
    if (length > 0) rd_record(line, MIN((size_t)length,sizeof(line)-1));
    /* Diagnostic mode only: let the guest's log exporter capture early faults. */
    rd_pause(50);
}
void rdxnu_linux_core_log(char *buffer, size_t size)
{
    lck_mtx_lock(&rd_log_lock);
    strlcpy(buffer,rd_log,size); lck_mtx_unlock(&rd_log_lock);
}

/* Preserve both the original size and alignment without relying on a Linux
 * allocator to free host allocations. Overflow and allocation failure propagate.
 */
struct rd_alloc { size_t total, offset; };
static void *rd_allocate(unsigned long size, size_t alignment)
{
    rd_trace("allocate");
    if (size > SIZE_MAX - alignment - sizeof(struct rd_alloc)) return NULL;
    size_t total = size + alignment + sizeof(struct rd_alloc);
    char *base = kalloc_data(total, Z_WAITOK | Z_ZERO);
    if (rd_tracing && size >= 1024 * 1024) {
        char line[128]; int n = snprintf(line,sizeof(line),"HOST allocated size=%lu base=%p\n",size,base);
        if (n > 0) rd_record(line,MIN((size_t)n,sizeof(line)-1));
        rd_pause(100);
    }
    if (!base) return NULL;
    uintptr_t address = ((uintptr_t)base + sizeof(struct rd_alloc) + alignment - 1) & ~(alignment - 1);
    struct rd_alloc *header = (struct rd_alloc *)address - 1;
    header->total = total; header->offset = address - (uintptr_t)base;
    return (void *)address;
}
static void *rd_alloc(unsigned long size) { return rd_allocate(size, 16); }
static void rd_free(void *ptr)
{
    if (!ptr) return;
    struct rd_alloc header = *((struct rd_alloc *)ptr - 1);
    char *base = (char *)ptr - header.offset;
    kfree_data(base, header.total);
}
static void *rd_page_alloc(unsigned long size) { return rd_allocate(size, PAGE_SIZE); }
static void rd_page_free(void *ptr, unsigned long size) { (void)size; rd_free(ptr); }
struct lkl_sem { semaphore_t value; };
static struct lkl_sem *rd_sem_alloc(int count)
{
    rd_trace("sem_alloc");
    struct lkl_sem *s = rd_alloc(sizeof(*s));
    if (s && semaphore_create(kernel_task, &s->value, SYNC_POLICY_FIFO, count) != KERN_SUCCESS) {
        rd_free(s); s = NULL;
    }
    return s;
}
static void rd_sem_free(struct lkl_sem *s) { semaphore_destroy(kernel_task, s->value); rd_free(s); }
static void rd_sem_up(struct lkl_sem *s)
{
    if (semaphore_signal(s->value) != KERN_SUCCESS) panic("rdxnu Linux semaphore signal");
}
static void rd_sem_down(struct lkl_sem *s)
{
    kern_return_t result;
    do { result = semaphore_wait(s->value); } while (result == KERN_ABORTED);
    if (result != KERN_SUCCESS) panic("rdxnu Linux semaphore wait: %d", result);
}
struct lkl_mutex { lck_mtx_t lock; thread_t owner; unsigned depth; int recursive; };
static struct lkl_mutex *rd_mutex_alloc(int recursive)
{
    rd_trace("mutex_alloc");
    struct lkl_mutex *m = rd_alloc(sizeof(*m));
    if (m) { lck_mtx_init(&m->lock, &rd_group, LCK_ATTR_NULL); m->recursive = recursive; }
    return m;
}
static void rd_mutex_free(struct lkl_mutex *m) { lck_mtx_destroy(&m->lock, &rd_group); rd_free(m); }
static void rd_mutex_lock(struct lkl_mutex *m)
{
    if (m->recursive && __atomic_load_n(&m->owner, __ATOMIC_RELAXED) == current_thread()) { m->depth++; return; }
    lck_mtx_lock(&m->lock); m->depth = 1;
    __atomic_store_n(&m->owner, current_thread(), __ATOMIC_RELAXED);
}
static void rd_mutex_unlock(struct lkl_mutex *m)
{
    if (__atomic_load_n(&m->owner, __ATOMIC_RELAXED) != current_thread() || !m->depth) panic("rdxnu Linux mutex ownership");
    if (--m->depth) return;
    __atomic_store_n(&m->owner, THREAD_NULL, __ATOMIC_RELAXED); lck_mtx_unlock(&m->lock);
}

struct lkl_tls_key { void (*destructor)(void *); };
struct rd_tls { struct rd_tls *next; struct lkl_tls_key *key; thread_t thread; void *value; };
static struct rd_tls *rd_tls_values;
struct rd_thread {
    struct rd_thread *next; thread_t thread;
    struct lkl_sem *gate, *done;
    void (*function)(void *); void *argument;
    bool detached, finished, joining;
};
static struct rd_thread *rd_threads;
static struct rd_thread *rd_find_thread(thread_t t)
{
    for (struct rd_thread *p = rd_threads; p; p = p->next) if (p->thread == t) return p;
    return NULL;
}
static void rd_unlink_thread(struct rd_thread *t)
{
    struct rd_thread **p = &rd_threads;
    while (*p && *p != t) p = &(*p)->next;
    if (*p) *p = t->next;
}
static void rd_release_thread(struct rd_thread *t)
{
    thread_deallocate(t->thread); rd_sem_free(t->gate); rd_sem_free(t->done); rd_free(t);
}
static void rd_tls_cleanup(void)
{
    /* Remove before invoking a destructor, which may itself enter host services. */
    for (;;) {
        lck_mtx_lock(&rd_registry);
        struct rd_tls **p = &rd_tls_values;
        while (*p && (*p)->thread != current_thread()) p = &(*p)->next;
        struct rd_tls *item = *p;
        void (*destructor)(void *) = item ? item->key->destructor : NULL;
        if (item) *p = item->next;
        lck_mtx_unlock(&rd_registry);
        if (!item) return;
        void *value = item->value; rd_free(item);
        if (value && destructor) destructor(value);
    }
}
static void rd_thread_exit(void)
{
    rd_tls_cleanup();
    lck_mtx_lock(&rd_registry);
    struct rd_thread *t = rd_find_thread(current_thread());
    if (!t) panic("rdxnu Linux exit outside host-created thread");
    t->finished = true;
    bool detached = t->detached;
    if (detached) rd_unlink_thread(t);
    else rd_sem_up(t->done);
    lck_mtx_unlock(&rd_registry);
    if (detached) rd_release_thread(t);
    thread_terminate_self();
    __builtin_unreachable();
}
static void rd_thread_entry(void *parameter, wait_result_t result)
{
    (void)result;
    struct rd_thread *t = parameter;
    rd_sem_down(t->gate); t->function(t->argument); rd_thread_exit();
}
static lkl_thread_t rd_thread_create(void (*function)(void *), void *argument)
{
    rd_trace("thread_create");
    struct rd_thread *t = rd_alloc(sizeof(*t));
    if (!t) return 0;
    t->gate = rd_sem_alloc(0); t->done = rd_sem_alloc(0);
    t->function = function; t->argument = argument;
    if (!t->gate || !t->done || kernel_thread_start(rd_thread_entry, t, &t->thread) != KERN_SUCCESS) {
        if (t->gate) rd_sem_free(t->gate);
        if (t->done) rd_sem_free(t->done);
        rd_free(t); return 0;
    }
    lkl_thread_t id = (lkl_thread_t)t->thread;
    lck_mtx_lock(&rd_registry); t->next = rd_threads; rd_threads = t; lck_mtx_unlock(&rd_registry);
    rd_sem_up(t->gate); return id;
}
static void rd_thread_detach(void)
{
    lck_mtx_lock(&rd_registry);
    struct rd_thread *t = rd_find_thread(current_thread());
    if (!t || t->joining) panic("rdxnu Linux invalid thread detach");
    t->detached = true; lck_mtx_unlock(&rd_registry);
}
static int rd_thread_join(lkl_thread_t id)
{
    lck_mtx_lock(&rd_registry);
    struct rd_thread *t = rd_find_thread((thread_t)id);
    if (!t || t->detached || t->joining || t->thread == current_thread()) { lck_mtx_unlock(&rd_registry); return -1; }
    t->joining = true; lck_mtx_unlock(&rd_registry);
    rd_sem_down(t->done);
    lck_mtx_lock(&rd_registry); rd_unlink_thread(t); lck_mtx_unlock(&rd_registry);
    rd_release_thread(t); return 0;
}
static lkl_thread_t rd_thread_self(void) { return (lkl_thread_t)current_thread(); }
static int rd_thread_equal(lkl_thread_t a, lkl_thread_t b) { return a == b; }
static void *rd_thread_stack(unsigned long *size)
{
    *size = kernel_stack_size; return (void *)thread_get_kernel_stack(current_thread());
}
static struct lkl_tls_key *rd_tls_alloc(void (*destructor)(void *))
{
    struct lkl_tls_key *key = rd_alloc(sizeof(*key));
    if (key) key->destructor = destructor;
    return key;
}
static void rd_tls_free(struct lkl_tls_key *key)
{
    lck_mtx_lock(&rd_registry);
    struct rd_tls **p = &rd_tls_values;
    while (*p) {
        struct rd_tls *item = *p;
        if (item->key == key) { *p = item->next; rd_free(item); } else p = &item->next;
    }
    lck_mtx_unlock(&rd_registry); rd_free(key);
}
static int rd_tls_set(struct lkl_tls_key *key, void *value)
{
    lck_mtx_lock(&rd_registry);
    for (struct rd_tls *p = rd_tls_values; p; p = p->next) {
        if (p->key == key && p->thread == current_thread()) { p->value = value; lck_mtx_unlock(&rd_registry); return 0; }
    }
    struct rd_tls *p = rd_alloc(sizeof(*p));
    if (p) { p->key = key; p->value = value; p->thread = current_thread(); p->next = rd_tls_values; rd_tls_values = p; }
    lck_mtx_unlock(&rd_registry); return p ? 0 : -1;
}
static void *rd_tls_get(struct lkl_tls_key *key)
{
    void *value = NULL;
    lck_mtx_lock(&rd_registry);
    for (struct rd_tls *p = rd_tls_values; p; p = p->next)
        if (p->key == key && p->thread == current_thread()) { value = p->value; break; }
    lck_mtx_unlock(&rd_registry); return value;
}
static unsigned long long rd_time(void)
{
    uint64_t absolute, ns; clock_get_uptime(&absolute); absolutetime_to_nanoseconds(absolute, &ns); return ns;
}
struct rd_timer { thread_call_t call; void (*function)(void); };
static void rd_timer_fire(thread_call_param_t first, thread_call_param_t second)
{
    (void)second; struct rd_timer *t = first; t->function();
}
static void *rd_timer_alloc(void (*function)(void))
{
    struct rd_timer *t = rd_alloc(sizeof(*t));
    if (t) { t->function = function; t->call = thread_call_allocate(rd_timer_fire, t); if (!t->call) { rd_free(t); t = NULL; } }
    return t;
}
static int rd_timer_set(void *timer, unsigned long delta)
{
    struct rd_timer *t = timer; uint64_t interval, deadline;
    nanoseconds_to_absolutetime(delta, &interval); clock_absolutetime_interval_to_deadline(interval, &deadline);
    thread_call_enter_delayed(t->call, deadline); return 0;
}
static void rd_timer_free(void *timer)
{
    struct rd_timer *t = timer; thread_call_cancel_wait(t->call);
    if (!thread_call_free(t->call)) panic("rdxnu Linux timer still active");
    rd_free(t);
}
static void rd_print(const char *str, int len) { if (len > 0) { rd_record(str,(size_t)len); printf("%.*s", len, str); } }
static void rd_panic(void) { panic("rdxnu embedded Linux panic at %s", rd_stage); }
int rdl_lkl_printf(const char *format, ...)
{
    char buffer[512]; va_list args; va_start(args, format);
    int n = vsnprintf(buffer, sizeof(buffer), format, args); va_end(args);
    rd_record(buffer,strlen(buffer)); printf("%s", buffer);
    if (rd_tracing) rd_pause(100);
    return n;
}
void rdl_lkl_bug(const char *format, ...)
{
    char buffer[512]; va_list args; va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args); va_end(args);
    panic("rdxnu Linux bug: %s", buffer);
}
static void *rd_ioremap(long address, int size) { (void)address; (void)size; return NULL; }
static int rd_iomem(const volatile void *address, void *value, int size, int write)
{ (void)address; (void)value; (void)size; (void)write; return -1; }
static struct lkl_host_operations rd_ops = {
    .virtio_devices = "", .print = rd_print, .panic = rd_panic,
    .sem_alloc = rd_sem_alloc, .sem_free = rd_sem_free, .sem_up = rd_sem_up, .sem_down = rd_sem_down,
    .mutex_alloc = rd_mutex_alloc, .mutex_free = rd_mutex_free, .mutex_lock = rd_mutex_lock, .mutex_unlock = rd_mutex_unlock,
    .thread_create = rd_thread_create, .thread_detach = rd_thread_detach, .thread_exit = rd_thread_exit,
    .thread_join = rd_thread_join, .thread_self = rd_thread_self, .thread_equal = rd_thread_equal, .thread_stack = rd_thread_stack,
    .tls_alloc = rd_tls_alloc, .tls_free = rd_tls_free, .tls_set = rd_tls_set, .tls_get = rd_tls_get,
    .mem_alloc = rd_alloc, .mem_free = rd_free, .page_alloc = rd_page_alloc, .page_free = rd_page_free,
    .time = rd_time, .timer_alloc = rd_timer_alloc, .timer_set_oneshot = rd_timer_set, .timer_free = rd_timer_free,
    .ioremap = rd_ioremap, .iomem_access = rd_iomem, .jmp_buf_set = rdxnu_linux_jump_set, .jmp_buf_longjmp = rdxnu_linux_jump_back,
    .memcpy = memcpy, .memset = memset, .memmove = memmove,
};

/* Raw numbers are the pinned LKL profile ABI, not an ELF syscall bridge. */
static long rd_call(long nr, long a, long b, long c, long d, long e, long f)
{ long args[6] = {a,b,c,d,e,f}; return rdl_lkl_syscall(nr, args); }
#define RD_CHECK(condition, label) do { if (!(condition)) { rd_stage = label; goto fail; } } while (0)
static void rd_smoke(void *parameter, wait_result_t result)
{
    (void)parameter; (void)result;
    rd_record("HOST worker entered\n",20);
    rd_pause(1000);
    rd_stage = "init";
    long value = rdl_lkl_init(&rd_ops);
    RD_CHECK(value == 0, "lkl_init");
    rd_record("HOST Linux init returned\n",25); rd_trace("init returned");
    rd_stage = "start"; value = rdl_lkl_start_kernel("");
    RD_CHECK(value == 0, "lkl_start_kernel");
    printf("RDXNU_LINUX_CORE_BOOT_PASS\n");
    int fds[2] = {-1,-1}; char data[16] = {0};
    value = rd_call(59, (long)fds, 04000,0,0,0,0); RD_CHECK(value == 0, "pipe2");
    value = rd_call(63,fds[0],(long)data,sizeof(data),0,0,0); RD_CHECK(value == -11, "EAGAIN");
    long ep = rd_call(20,0,0,0,0,0,0); RD_CHECK(ep >= 0, "epoll create");
    struct { uint32_t events, padding; uint64_t data; } event = {1,0,0x5244584e55ULL}, ready = {0};
    value = rd_call(21,ep,1,fds[0],(long)&event,0,0); RD_CHECK(value == 0, "epoll ctl");
    value = rd_call(22,ep,(long)&ready,1,0,0,0); RD_CHECK(value == 0, "epoll empty");
    const char payload[] = "Linux on XNU";
    value = rd_call(64,fds[1],(long)payload,sizeof(payload),0,0,0); RD_CHECK(value == sizeof(payload), "pipe write");
    value = rd_call(22,ep,(long)&ready,1,1000,0,0); RD_CHECK(value == 1 && ready.data == event.data && (ready.events & 1), "epoll ready");
    value = rd_call(63,fds[0],(long)data,sizeof(data),0,0,0); RD_CHECK(value == sizeof(payload) && !memcmp(data,payload,sizeof(payload)), "pipe read");
    value = rd_call(22,ep,(long)&ready,1,0,0,0); RD_CHECK(value == 0, "epoll drain");
    value = rd_call(57,fds[1],0,0,0,0,0); RD_CHECK(value == 0, "close writer");
    value = rd_call(63,fds[0],(long)data,sizeof(data),0,0,0); RD_CHECK(value == 0, "EOF");
    RD_CHECK(rd_call(57,fds[0],0,0,0,0,0) == 0 && rd_call(57,ep,0,0,0,0,0) == 0, "close pipe");
    RD_CHECK(rd_call(63,-1,(long)data,1,0,0,0) == -9, "EBADF");
    printf("RDXNU_LINUX_CORE_PIPE_EPOLL_PASS\n");
    value = rd_call(34,-100,(long)"/xnu-core-smoke",0700,0,0,0); RD_CHECK(value == 0, "mkdirat");
    long fd = rd_call(56,-100,(long)"/xnu-core-smoke/data",0302,0600,0,0); RD_CHECK(fd >= 0, "openat");
    RD_CHECK(rd_call(64,fd,(long)payload,sizeof(payload),0,0,0) == sizeof(payload), "file write");
    RD_CHECK(rd_call(62,fd,0,0,0,0,0) == 0, "lseek"); memset(data,0,sizeof(data));
    value = rd_call(63,fd,(long)data,sizeof(data),0,0,0); RD_CHECK(value == sizeof(payload) && !memcmp(data,payload,sizeof(payload)), "file read");
    RD_CHECK(rd_call(57,fd,0,0,0,0,0) == 0 && rd_call(35,-100,(long)"/xnu-core-smoke/data",0,0,0,0) == 0 && rd_call(35,-100,(long)"/xnu-core-smoke",0x200,0,0,0) == 0, "VFS cleanup");
    printf("RDXNU_LINUX_CORE_VFS_PASS\n");
    rd_stage = "halt"; value = rdl_lkl_sys_halt();
    RD_CHECK(value == 0, "Linux halt");
    rdl_lkl_cleanup(); rd_tls_cleanup();
    rd_stage = "passed"; __atomic_store_n(&rd_status,2,__ATOMIC_RELEASE);
    printf("RDXNU_LINUX_CORE_SMOKE_PASS\n"); thread_terminate_self(); return;
fail:
    /* Keep failed core state for diagnosis; never pretend it can be restarted. */
    printf("RDXNU_LINUX_CORE_FAIL stage=%s result=%ld\n",rd_stage,value);
    __atomic_store_n(&rd_status,-1,__ATOMIC_RELEASE); thread_terminate_self();
}
int rdxnu_linux_core_status(void) { return __atomic_load_n(&rd_status,__ATOMIC_ACQUIRE); }
int rdxnu_linux_core_start(void)
{
    int enabled = 0, expected = 0;
    if (!PE_parse_boot_argn("rdxnu_linux_core", &enabled, sizeof(enabled)) || enabled != 1) return -1;
    if (!__atomic_compare_exchange_n(&rd_status,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) return -2;
    int tracing = 0; PE_parse_boot_argn("rdxnu_linux_core_trace", &tracing, sizeof(tracing)); rd_tracing = tracing == 1;
    thread_t worker;
    if (kernel_thread_start(rd_smoke,NULL,&worker) != KERN_SUCCESS) { __atomic_store_n(&rd_status,-2,__ATOMIC_RELEASE); return -3; }
    thread_deallocate(worker); return 0;
}
