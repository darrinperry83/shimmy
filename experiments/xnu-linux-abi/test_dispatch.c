/* Test the actual experiment header with a synthetic saved state.
 * This is a host unit test, not evidence of a booted or compiled XNU kernel.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint64_t x[31], esr, pc, sp;
    uint32_t cpsr;
    bool is64;
} arm_saved_state_t;
struct proc { int pid; };
static int boot_value, boot_present, parser_calls;
static bool is_saved_state64(const arm_saved_state_t *s) { return s->is64; }
static uint64_t get_saved_state_esr(const arm_saved_state_t *s) { return s->esr; }
static uint64_t get_saved_state_reg(const arm_saved_state_t *s, unsigned r) { return s->x[r]; }
static void set_saved_state_reg(arm_saved_state_t *s, unsigned r, uint64_t v) { s->x[r] = v; }
static int proc_pid(struct proc *p) { return p->pid; }
static bool PE_parse_boot_argn(const char *name, void *out, int size)
{
    assert(!strcmp(name, "rdxnu_linux_probe") && size == sizeof(int));
    parser_calls++;
    if (boot_present) memcpy(out, &boot_value, size);
    return boot_present;
}
#include "rdxnu_linux_probe.h"

static arm_saved_state_t state(void)
{
    arm_saved_state_t s = {0};
    for (unsigned i = 0; i < 31; i++) s.x[i] = 0x1000 + i;
    s.x[8] = 172;
    s.x[16] = RDXNU_PROBE_MARKER;
    s.esr = (uint64_t)0x15 << 26; /* ESR_EC_SVC_64, immediate zero */
    s.pc = 0x100004004; /* already points after the SVC */
    s.sp = 0x120000000;
    s.cpsr = 0xf0000000;
    s.is64 = true;
    return s;
}

static void unchanged(arm_saved_state_t s, struct proc *p)
{
    arm_saved_state_t before = s;
    assert(!rdxnu_linux_probe(&s, p));
    assert(!memcmp(&s, &before, sizeof(s)));
}

int main(void)
{
    struct proc p = {.pid = 4242};
    boot_present = boot_value = 1;
    arm_saved_state_t s = state(), before = s;
#if DEBUG || DEVELOPMENT
    assert(rdxnu_linux_probe(&s, &p));
    assert(s.x[0] == 4242);
    before.x[0] = 4242;
    assert(!memcmp(&s, &before, sizeof(s))); /* all other state preserved */
    s = state(); s.x[8] = UINT64_MAX; before = s;
    assert(rdxnu_linux_probe(&s, &p));
    assert((int64_t)s.x[0] == -38);
    before.x[0] = (uint64_t)-38;
    assert(!memcmp(&s, &before, sizeof(s)));
    s = state(); s.x[8] = (UINT64_C(1) << 32) | 172;
    assert(rdxnu_linux_probe(&s, &p));
    assert((int64_t)s.x[0] == -38); /* number must not be truncated to 32 bits */
    s = state(); s.x[16] = 20;
    int calls = parser_calls;
    unchanged(s, &p);
    assert(calls == parser_calls); /* ordinary Darwin avoids the parser */
    s = state(); s.esr |= 0x80; unchanged(s, &p);
    s = state(); s.is64 = false; unchanged(s, &p);
    s = state(); boot_present = 0; unchanged(s, &p);
    boot_present = 1; boot_value = 0; unchanged(s, &p);
    boot_value = 2; unchanged(s, &p);
    puts("DISPATCH_MODEL_PASS: gates, PID, Linux errno, preserved state");
#else
    (void)before;
    unchanged(s, &p);
    assert(!parser_calls);
    puts("RELEASE_DISABLED_PASS");
#endif
}
