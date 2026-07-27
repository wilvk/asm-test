/*
 * test_regstate_parity.c — 26 T5.2, the load-bearing cross-producer correctness
 * check for the live `regstate` register ring.
 *
 * Run ONE deterministic routine through BOTH per-step register producers:
 *   - the trusted EMULATOR ring (emu_step_capture, the existing `--steps` golden
 *     path), whose guest code sits at the fixed EMU_CODE_BASE, and
 *   - the LIVE single-step PTRACE producer (asmtest_dataflow_ptrace_run with the
 *     regfile ring armed — this brief's new capture), running in a real forked
 *     tracee at an ASLR'd address,
 * and assert the two per-step register files AGREE on the operand-visible
 * computation registers (rax / rdi / rsi), which are base-INDEPENDENT for a
 * pure-arithmetic routine. The base-DEPENDENT registers (rip / rsp / rbp) differ by
 * construction — the emulator's fixed base vs the tracee's real ASLR — so they are
 * NOT required to match; instead we assert rip actually DIFFERS, proving the
 * agreement is a genuine cross-basis result and not two identical captures.
 *
 * This is the parity the Scrubber relies on to render both producers from one deck:
 * the live ring must carry the SAME value semantics as the trusted emulator ring,
 * differing only by base offsets.
 *
 * x86-64-guest only (the ptrace producer single-steps x86-64; the emulator corpus
 * is host-arch too). The mk rule gates the build on x86_64 + libunicorn; a run-time
 * ptrace refusal (seccomp) or an off-arch producer self-skips honestly.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asmtest_emu.h"      /* emu_* + EMU_CODE_BASE + emu_x86_regs_t */
#include "asmtest_valtrace.h" /* asmtest_valtrace_t + asmtest_regfile_t */

/* Both producers are forward-declared inline by their in-tree callers (neither is
 * in a public header; tools/asmtrace_record.c and examples/ declare them the same
 * way). */
int asmtest_dataflow_emu_run(const uint8_t *code, size_t code_len,
                             const long *args, int nargs, uint64_t max_insns,
                             asmtest_valtrace_t *vt);
int asmtest_dataflow_ptrace_run(const uint8_t *code, size_t code_len,
                                const long *args, int nargs, uint64_t max_insns,
                                uint64_t gs_base, long *result,
                                asmtest_valtrace_t *vt);

/* The ptrace producer's return codes (cli/asmspy_engine.c; not in a header). */
#define DFP_OK 0
#define DFP_FAULT 1
#define DFP_ENOSYS (-3) /* off x86-64 / no Capstone: self-skip */
#define DFP_ETRACE (-4) /* SEIZE/ptrace/wait failure (seccomp): self-skip */

static int fails = 0;
#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            fprintf(stderr, "FAIL: ");                                          \
            fprintf(stderr, __VA_ARGS__);                                       \
            fprintf(stderr, "\n");                                              \
            fails++;                                                            \
        }                                                                       \
    } while (0)

int main(void) {
    /* sq(a,b) = (a+b)^2 - a — pure integer arithmetic threading rax from the args
     * rdi, rsi, with no memory and no stack beyond the call frame, so rax/rdi/rsi
     * are functions of the inputs alone and identical in either basis. */
    static const uint8_t ROUTINE[] = {
        0x48, 0x89, 0xf8,       /* 0x00 mov  rax, rdi */
        0x48, 0x01, 0xf0,       /* 0x03 add  rax, rsi */
        0x48, 0x0f, 0xaf, 0xc0, /* 0x06 imul rax, rax */
        0x48, 0x29, 0xf8,       /* 0x0a sub  rax, rdi */
        0xc3,                   /* 0x0d ret          */
    };
    const long args[2] = {6, 7};
    const long expect = (6L + 7L) * (6L + 7L) - 6L; /* 163 */

    /* --- the trusted emulator ring ------------------------------------- */
    emu_x86_regs_t emu[64];
    size_t enh = 0;
    {
        emu_t *e = emu_open();
        CHECK(e != NULL, "emu_open failed");
        if (e == NULL)
            return 1;
        CHECK(emu_step_capture(e, 64), "emu_step_capture failed");
        emu_result_t res;
        memset(&res, 0, sizeof res);
        emu_call(e, ROUTINE, sizeof ROUTINE, args, 2, 0, &res);
        CHECK((long)res.regs.rax == expect, "emulator result rax=%lld, want %ld",
              (long long)res.regs.rax, expect);
        enh = emu_step_count(e);
        for (size_t i = 0; i < enh && i < 64; i++)
            emu_step_at(e, i, NULL, &emu[i]);
        emu_close(e);
    }
    CHECK(enh >= 4, "emulator captured only %zu steps", enh);

    /* --- the live ptrace ring ------------------------------------------ */
    asmtest_valtrace_t *vt = asmtest_valtrace_new(64, 512, 512);
    CHECK(vt != NULL, "valtrace_new failed");
    if (vt == NULL)
        return 1;
    CHECK(asmtest_valtrace_arm_regfile(vt), "arm_regfile failed");
    long result = 0;
    int rc = asmtest_dataflow_ptrace_run(ROUTINE, sizeof ROUTINE, args, 2, 0, 0,
                                         &result, vt);
    if (rc == DFP_ENOSYS || rc == DFP_ETRACE) {
        printf("# SKIP test_regstate_parity: live ptrace producer unavailable "
               "(rc=%d: off x86-64 / no Capstone / ptrace refused)\n",
               rc);
        asmtest_valtrace_free(vt);
        return 0;
    }
    CHECK(rc == DFP_OK || rc == DFP_FAULT, "ptrace _run rc=%d", rc);
    CHECK(result == expect, "live result=%ld, want %ld", result, expect);
    CHECK(vt->regfile != NULL, "live regfile ring not armed / not filled");
    CHECK(vt->steps_len >= 4, "live captured only %zu steps", vt->steps_len);

    /* --- parity: computation registers agree, basis registers differ ---- */
    size_t n = enh < vt->steps_len ? enh : vt->steps_len;
    size_t compared = 0, rip_differs = 0;
    for (size_t i = 0; i < n; i++) {
        uint64_t emu_off = emu[i].rip - EMU_CODE_BASE;
        uint64_t live_off = vt->insn_off[i];
        if (emu_off != live_off)
            break; /* streams diverged (e.g. an emu trampoline step): stop */
        if (live_off >= sizeof ROUTINE)
            break; /* stepped out of the routine's own bytes */
        /* Base-dependent: only require the two are genuinely on different bases. */
        if (emu[i].rip != vt->regfile[i].rip)
            rip_differs++;
        /* Operand-visible computation registers are base-INDEPENDENT and must
         * agree exactly at every aligned step. rdi/rsi are args, set before the
         * routine, so compare from step 0; rax is garbage until step 0 writes it
         * (the emulator zeroes GP regs, the live tracee does not), so compare it
         * only from step 1 onward. */
        CHECK(emu[i].rdi == vt->regfile[i].rdi, "step %zu rdi: emu=%llu live=%llu",
              i, (unsigned long long)emu[i].rdi,
              (unsigned long long)vt->regfile[i].rdi);
        CHECK(emu[i].rsi == vt->regfile[i].rsi, "step %zu rsi: emu=%llu live=%llu",
              i, (unsigned long long)emu[i].rsi,
              (unsigned long long)vt->regfile[i].rsi);
        if (i >= 1)
            CHECK(emu[i].rax == vt->regfile[i].rax,
                  "step %zu rax: emu=%llu live=%llu", i,
                  (unsigned long long)emu[i].rax,
                  (unsigned long long)vt->regfile[i].rax);
        compared++;
    }
    CHECK(compared >= 4, "only %zu aligned steps compared (want >= 4)", compared);
    CHECK(rip_differs > 0,
          "rip never differed — the two captures share a basis, so the "
          "computation-register agreement is not a cross-basis result");

    asmtest_valtrace_free(vt);
    if (fails) {
        fprintf(stderr, "test_regstate_parity: %d check(s) FAILED\n", fails);
        return 1;
    }
    printf("test_regstate_parity: OK (%zu aligned steps; rax/rdi/rsi agree with "
           "the emulator modulo base, rip differs)\n",
           compared);
    return 0;
}
