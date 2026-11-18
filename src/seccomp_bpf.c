#include "seccomp_bpf.h"
#include <string.h>

#define SHIM_BPF_MAX_INSNS 4096u
#define SHIM_BPF_LD_W_ABS  0x20u
#define SHIM_BPF_LD_IMM    0x00u
#define SHIM_BPF_LDX_IMM   0x01u
#define SHIM_BPF_AND_K     0x54u
#define SHIM_BPF_OR_K      0x44u
#define SHIM_BPF_XOR_K     0xa4u
#define SHIM_BPF_ADD_K     0x04u
#define SHIM_BPF_SUB_K     0x14u
#define SHIM_BPF_JA        0x05u
#define SHIM_BPF_JEQ_K     0x15u
#define SHIM_BPF_JGT_K     0x25u
#define SHIM_BPF_JGE_K     0x35u
#define SHIM_BPF_JSET_K    0x45u
#define SHIM_BPF_RET_K     0x06u
#define SHIM_BPF_TAX       0x07u
#define SHIM_BPF_TXA       0x87u

_Static_assert(sizeof(struct shim_sock_filter) == 8, "Linux sock_filter ABI");
_Static_assert(sizeof(struct shim_seccomp_data) == 64, "Linux seccomp_data ABI");

static int return_supported(uint32_t action) {
  return action == SHIM_SECCOMP_RET_ALLOW ||
         (action & 0xffff0000u) == SHIM_SECCOMP_RET_ERRNO;
}

int shim_seccomp_bpf_verify(const struct shim_sock_filter *p, size_t n) {
  if (!p || !n || n > SHIM_BPF_MAX_INSNS || p[n - 1].code != SHIM_BPF_RET_K)
    return -1;
  for (size_t i = 0; i < n; ++i) {
    const struct shim_sock_filter *f = &p[i];
    switch (f->code) {
      case SHIM_BPF_LD_W_ABS:
        if (f->k > sizeof(struct shim_seccomp_data) - 4 || (f->k & 3) ||
            (f->k >= 8 && f->k < 16)) return -1; /* guest IP is not plumbed yet */
        break;
      case SHIM_BPF_LD_IMM: case SHIM_BPF_LDX_IMM:
      case SHIM_BPF_AND_K: case SHIM_BPF_OR_K: case SHIM_BPF_XOR_K:
      case SHIM_BPF_ADD_K: case SHIM_BPF_SUB_K:
        break;
      case SHIM_BPF_JA:
        if (f->k >= n - i - 1 || f->jt || f->jf) return -1;
        break;
      case SHIM_BPF_JEQ_K: case SHIM_BPF_JGT_K:
      case SHIM_BPF_JGE_K: case SHIM_BPF_JSET_K:
        if (f->jt >= n - i - 1 || f->jf >= n - i - 1) return -1;
        break;
      case SHIM_BPF_RET_K:
        if (!return_supported(f->k)) return -1;
        break;
      case SHIM_BPF_TAX: case SHIM_BPF_TXA:
        if (f->k) return -1;
        break;
      default:
        return -1; /* No unchecked classic-BPF opcodes. */
    }
    if (f->code != SHIM_BPF_JEQ_K && f->code != SHIM_BPF_JGT_K &&
        f->code != SHIM_BPF_JGE_K && f->code != SHIM_BPF_JSET_K &&
        (f->jt || f->jf)) return -1;
  }
  return 0;
}

uint32_t shim_seccomp_bpf_eval(const struct shim_sock_filter *p, size_t n,
                              const struct shim_seccomp_data *data) {
  const uint32_t deny = SHIM_SECCOMP_RET_ERRNO | 1u; /* EPERM */
  if (!data || shim_seccomp_bpf_verify(p, n) != 0) return deny;
  return shim_seccomp_bpf_eval_verified(p, n, data);
}

uint32_t shim_seccomp_bpf_eval_verified(const struct shim_sock_filter *p, size_t n,
                                       const struct shim_seccomp_data *data) {
  const uint32_t deny = SHIM_SECCOMP_RET_ERRNO | 1u;
  if (!p || !data || !n || n > SHIM_BPF_MAX_INSNS) return deny;
  uint32_t a = 0, x = 0;
  for (size_t pc = 0; pc < n;) {
    const struct shim_sock_filter *f = &p[pc];
    size_t next = pc + 1;
    switch (f->code) {
      case SHIM_BPF_LD_W_ABS:
        memcpy(&a, (const unsigned char *)data + f->k, sizeof a); break;
      case SHIM_BPF_LD_IMM: a = f->k; break;
      case SHIM_BPF_LDX_IMM: x = f->k; break;
      case SHIM_BPF_AND_K: a &= f->k; break;
      case SHIM_BPF_OR_K: a |= f->k; break;
      case SHIM_BPF_XOR_K: a ^= f->k; break;
      case SHIM_BPF_ADD_K: a += f->k; break;
      case SHIM_BPF_SUB_K: a -= f->k; break;
      case SHIM_BPF_JA: next += f->k; break;
      case SHIM_BPF_JEQ_K: next += a == f->k ? f->jt : f->jf; break;
      case SHIM_BPF_JGT_K: next += a > f->k ? f->jt : f->jf; break;
      case SHIM_BPF_JGE_K: next += a >= f->k ? f->jt : f->jf; break;
      case SHIM_BPF_JSET_K: next += (a & f->k) ? f->jt : f->jf; break;
      case SHIM_BPF_RET_K: return f->k;
      case SHIM_BPF_TAX: x = a; break;
      case SHIM_BPF_TXA: a = x; break;
      default: return deny; /* verifier and evaluator must agree */
    }
    pc = next;
  }
  return deny;
}
