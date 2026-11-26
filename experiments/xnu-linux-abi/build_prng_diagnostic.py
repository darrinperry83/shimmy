#!/usr/bin/env python3
"""Build VM-only, late-call PRNG checkpoints without altering baseline artifacts.

Temporarily instruments pinned XNU sources under the shared build lock and
restores their exact bytes afterwards. Never installs a boot configuration.
Select a stopping stage with rdxnu_prng_stage; default 105 precedes the DRBG
callback. Checkpoints remain disarmed during the earlier successful RNG call.
"""
import fcntl
import hashlib
import json
from pathlib import Path
import subprocess
import prepare

HERE = Path(__file__).resolve().parent
OUT = HERE / 'out'
XNU = OUT / 'xnu'
TARGET = OUT / 'vmapple/prng-diagnostic'

HEADER = r'''
#include <pexpert/arm64/board_config.h>
#if !APPLEVIRTUALPLATFORM
#error PRNG checkpoints are restricted to VMAPPLE
#endif
extern volatile unsigned int rdxnu_prng_stage;
static inline void rdxnu_prng_mark(unsigned int stage)
{
    if (rdxnu_prng_stage == stage) {
        __asm__ volatile("msr daifset, #15\n"
                         "mov w0, #8\nmovk w0, #0x8400, lsl #16\n"
                         "mov x1, #0\nmov x2, #0\nmov x3, #0\n"
                         "hvc #0\n" ::: "x0", "x1", "x2", "x3", "memory");
        for (;;) { __asm__ volatile("wfe"); }
    }
}
'''


def replace_once(text, before, after):
    if text.count(before) != 1:
        raise ValueError(f'expected one source anchor: {before[:80]!r}')
    return text.replace(before, after, 1)


def instrument():
    paths = ['osfmk/arm/arm_init.c', 'osfmk/prng/prng_random.c',
             'osfmk/corecrypto/ccdrbg.c', 'osfmk/corecrypto/ccdrbg_nisthmac.c', prepare.SLEH]
    saved = {p: (XNU / p).read_bytes() for p in paths}
    changed = {p: b.decode() for p, b in saved.items()}
    p = 'osfmk/arm/arm_init.c'
    changed[p] = replace_once(changed[p],
        '\t__stack_chk_guard = (unsigned long)early_random();',
        '\textern void rdxnu_prng_arm(void);\n\trdxnu_prng_arm();\n'
        '\t__stack_chk_guard = (unsigned long)early_random();')
    p = 'osfmk/prng/prng_random.c'
    changed[p] = replace_once(changed[p], 'static struct cckprng_ctx *prng_ctx;', HEADER + '''
volatile unsigned int rdxnu_prng_stage;
void rdxnu_prng_arm(void)
{
    unsigned int stage = 105;
    PE_parse_boot_argn("rdxnu_prng_stage", &stage, sizeof(stage));
    rdxnu_prng_stage = stage;
}
static struct cckprng_ctx *prng_ctx;''')
    changed[p] = replace_once(changed[p], 'early_random(void)\n{',
                            'early_random(void)\n{\n\trdxnu_prng_mark(100);')
    changed[p] = replace_once(changed[p], '\tread_erandom(&result, sizeof(result));',
        '\trdxnu_prng_mark(101);\n\tread_erandom(&result, sizeof(result));\n\trdxnu_prng_mark(109);')
    changed[p] = replace_once(changed[p], '\t\tread_random_generate(buf, nbytes);',
        '\t\trdxnu_prng_mark(103);\n\t\tread_random_generate(buf, nbytes);')
    changed[p] = replace_once(changed[p], '\t\tread_erandom_generate(buf, nbytes);',
        '\t\trdxnu_prng_mark(102);\n\t\tread_erandom_generate(buf, nbytes);')
    changed[p] = replace_once(changed[p], '\t\trc = ccdrbg_generate(',
        '\t\trdxnu_prng_mark(104);\n\t\trc = ccdrbg_generate(')
    p = 'osfmk/corecrypto/ccdrbg.c'
    changed[p] = replace_once(changed[p], '#include "ccdrbg_internal.h"',
                            '#include "ccdrbg_internal.h"\n' + HEADER)
    changed[p] = replace_once(changed[p],
        '\treturn info->generate(drbg, dataOutLength, dataOut, additionalLength, additional);',
        '\trdxnu_prng_mark(105);\n'
        '\tint result = info->generate(drbg, dataOutLength, dataOut, additionalLength, additional);\n'
        '\trdxnu_prng_mark(106);\n\treturn result;')
    p = 'osfmk/corecrypto/ccdrbg_nisthmac.c'
    changed[p] = replace_once(changed[p], 'static void\ndone(', HEADER + '\nstatic void\ndone(')
    changed[p] = replace_once(changed[p],
        'generate(struct ccdrbg_state *ctx, size_t out_nbytes, void *out, size_t add_nbytes, const void *add)\n{',
        'generate(struct ccdrbg_state *ctx, size_t out_nbytes, void *out, size_t add_nbytes, const void *add)\n{\n\trdxnu_prng_mark(107);')
    changed[p] = replace_once(changed[p], '\tint status = CCDRBG_STATUS_PARAM_ERROR;\n\tcc_require(out_nbytes',
        '\trdxnu_prng_mark(108);\n\tint status = CCDRBG_STATUS_PARAM_ERROR;\n\tcc_require(out_nbytes')
    changed[p] = replace_once(changed[p], '\tuint8_t Vprev[DRBG_HMAC_MAX_OUTPUT_SIZE];',
        '\trdxnu_prng_mark(110);\n\tuint8_t Vprev[DRBG_HMAC_MAX_OUTPUT_SIZE];\n\trdxnu_prng_mark(111);')
    changed[p] = replace_once(changed[p], '\t\tcc_memcpy(Vprev, drbg_ctx->V, outlen);',
        '\t\tcc_memcpy(Vprev, drbg_ctx->V, outlen);\n\t\trdxnu_prng_mark(112);')
    changed[p] = replace_once(changed[p], '\t\tcchmac(info, outlen, drbg_ctx->key, outlen, drbg_ctx->V, drbg_ctx->V);\n\n\t\t// See FIPS',
        '\t\tcchmac(info, outlen, drbg_ctx->key, outlen, drbg_ctx->V, drbg_ctx->V);\n\t\trdxnu_prng_mark(113);\n\n\t\t// See FIPS')
    changed[p] = replace_once(changed[p], '\tupdate(ctx, 1, add_nbytes, add);\n\n\tdrbg_ctx->reseed_counter += 1;',
        '\trdxnu_prng_mark(114);\n\tupdate(ctx, 1, add_nbytes, add);\n\trdxnu_prng_mark(115);\n\n\tdrbg_ctx->reseed_counter += 1;')
    changed[p] = replace_once(changed[p], 'update(struct ccdrbg_state *ctx, unsigned ndata, ...)\n{',
        'update(struct ccdrbg_state *ctx, unsigned ndata, ...)\n{\n\trdxnu_prng_mark(120);')
    changed[p] = replace_once(changed[p], '\tcchmac_di_decl(info, hmac_ctx);',
        '\trdxnu_prng_mark(121);\n\tcchmac_di_decl(info, hmac_ctx);\n\trdxnu_prng_mark(122);')
    changed[p] = replace_once(changed[p], '\t\tcchmac_init(info, hmac_ctx, outlen, drbg_ctx->key);',
        '\t\tcchmac_init(info, hmac_ctx, outlen, drbg_ctx->key);\n\t\trdxnu_prng_mark(123);')
    changed[p] = replace_once(changed[p], '\t\tcchmac_update(info, hmac_ctx, outlen, drbg_ctx->V);',
        '\t\tcchmac_update(info, hmac_ctx, outlen, drbg_ctx->V);\n\t\trdxnu_prng_mark(124);')
    changed[p] = replace_once(changed[p], '\t\tcchmac_update(info, hmac_ctx, sizeof(b), &b);',
        '\t\tcchmac_update(info, hmac_ctx, sizeof(b), &b);\n\t\trdxnu_prng_mark(125);')
    changed[p] = replace_once(changed[p], '\t\tva_end(args);',
        '\t\tva_end(args);\n\t\trdxnu_prng_mark(126);')
    changed[p] = replace_once(changed[p], '\t\tcchmac_final(info, hmac_ctx, drbg_ctx->key);',
        '\t\tcchmac_final(info, hmac_ctx, drbg_ctx->key);\n\t\trdxnu_prng_mark(127);')
    changed[p] = replace_once(changed[p], '\tcchmac_di_clear(info, hmac_ctx);',
        '\trdxnu_prng_mark(128);\n\tcchmac_di_clear(info, hmac_ctx);\n\trdxnu_prng_mark(129);')
    original = subprocess.check_output(['git', '-C', str(XNU), 'show', f'HEAD:{prepare.SLEH}'])
    if saved[prepare.SLEH] != prepare.patched(original).encode():
        raise ValueError('unexpected SVC source')
    changed[prepare.SLEH] = original.decode()
    return saved, {p: text.encode() for p, text in changed.items()}


def main():
    TARGET.mkdir(parents=True, exist_ok=True)
    with (OUT / 'kernel-build.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        saved, changed = instrument()
        command = json.loads((OUT / 'vmapple/baseline-no-lto-release/build-result.json').read_text())['command']
        command = [arg for arg in command if not arg.startswith(('OBJROOT=', 'SYMROOT=', 'DSTROOT='))]
        command += [f'OBJROOT={TARGET / "obj"}', f'SYMROOT={TARGET / "sym"}', f'DSTROOT={TARGET / "dst"}']
        report = {'command': command, 'state': 'building', 'default_stage': 105,
                  'source_restored': False, 'booted': False, 'svc_probe_present': False}
        result = TARGET / 'build-result.json'
        try:
            for p, data in changed.items():
                (TARGET / 'sources' / p).parent.mkdir(parents=True, exist_ok=True)
                (TARGET / 'sources' / p).write_bytes(data)
                (XNU / p).write_bytes(data)
            result.write_text(json.dumps(report, indent=2) + '\n')
            with (TARGET / 'build.log').open('w') as log:
                code = subprocess.run(command, cwd=XNU, stdout=log, stderr=subprocess.STDOUT).returncode
            report.update(returncode=code, state='built' if code == 0 else 'failed')
            if code:
                raise RuntimeError('diagnostic build failed; see build.log')
            kernel = TARGET / 'obj/RELEASE_ARM64_VMAPPLE/kernel.release.vmapple'
            report.update(kernel=str(kernel), kernel_sha256=hashlib.sha256(kernel.read_bytes()).hexdigest())
        finally:
            for p in saved:
                if (XNU / p).read_bytes() != changed[p]:
                    raise RuntimeError(f'concurrent source edit: {p}; refusing overwrite')
                (XNU / p).write_bytes(saved[p])
            report['source_restored'] = all((XNU / p).read_bytes() == b for p, b in saved.items())
            result.write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
