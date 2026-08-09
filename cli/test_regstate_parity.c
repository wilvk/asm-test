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
 * ptrace refusal (seccomp) or an off-arch producer self-skips transparently.
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
#define DFP_OK     0
#define DFP_FAULT  1
#define DFP_ENOSYS (-3) /* off x86-64 / no Capstone: self-skip */
#define DFP_ETRACE (-4) /* SEIZE/ptrace/wait failure (seccomp): self-skip */

static int fails = 0;
#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL: ");                                         \
            fprintf(stderr, __VA_ARGS__);                                      \
            fprintf(stderr, "\n");                                             \
            fails++;                                                           \
        }                                                                      \
    } while (0)

/* R4 (31-wide-register-deck.md T1): the WIDE-DECK parity — one XMM-writing routine
 * through BOTH the emulator ring and the live ptrace ring with the FP/vector deck
 * armed (`asmtest_valtrace_arm_fpregs`), asserting their per-step XMM0 files agree
 * once the routine has written it. XMM0 is (double)rdi — a function of the INTEGER
 * arg alone, so it is base-INDEPENDENT (the live producer needs no FP-arg
 * marshalling), and the two producers must byte-agree modulo the untouched entry
 * state. Skips transparently (no fail) when the live producer is unavailable. */
static void vec_parity(void) {
    /* movq xmm0, rdi ; ret — writes rdi into xmm0's low 64 bits and ZERO-EXTENDS
     * the upper 64 (that is `movq r/m64 -> xmm`'s defined behaviour), so the WHOLE
     * 128-bit register is a function of rdi alone. A scalar op like cvtsi2sd would
     * leave the upper lane at its entry state — zero in the emulator (it zeroes the
     * vector file per call) but garbage in the live tracee (it does not), a
     * base-DEPENDENT difference like rsp. movq removes that ambiguity so the full
     * register is base-INDEPENDENT and the two decks must byte-agree over all 16. */
    static const uint8_t VEC[] = {
        0x66, 0x48, 0x0f, 0x6e, 0xc7, /* movq xmm0, rdi */
        0xc3,                         /* ret            */
    };
    const long args[1] = {42};
    const uint64_t want = 42; /* xmm0.u64[0] after the zero-extending move */

    /* --- the trusted emulator ring, XMM captured every step --- */
    emu_x86_regs_t emu[16];
    size_t enh = 0;
    {
        emu_t *e = emu_open();
        CHECK(e != NULL, "emu_open failed (vec)");
        if (e == NULL)
            return;
        CHECK(emu_step_capture(e, 16), "emu_step_capture failed (vec)");
        emu_result_t res;
        memset(&res, 0, sizeof res);
        emu_call(e, VEC, sizeof VEC, args, 1, 0, &res);
        CHECK(res.regs.xmm[0].u64[0] == want && res.regs.xmm[0].u64[1] == 0,
              "emu vec xmm0.lo=%llu (want %llu), .hi=%llu (want 0)",
              (unsigned long long)res.regs.xmm[0].u64[0],
              (unsigned long long)want,
              (unsigned long long)res.regs.xmm[0].u64[1]);
        enh = emu_step_count(e);
        for (size_t i = 0; i < enh && i < 16; i++)
            emu_step_at(e, i, NULL, &emu[i]);
        emu_close(e);
    }
    CHECK(enh >= 2, "emulator vec captured only %zu steps", enh);

    /* --- the live ptrace ring, FP/vector deck armed --- */
    asmtest_valtrace_t *vt = asmtest_valtrace_new(16, 128, 256);
    CHECK(vt != NULL, "valtrace_new failed (vec)");
    if (vt == NULL)
        return;
    CHECK(asmtest_valtrace_arm_fpregs(vt), "arm_fpregs failed");
    CHECK(vt->regfile_fp, "regfile_fp not set after arm_fpregs");
    long result = 0;
    int rc = asmtest_dataflow_ptrace_run(VEC, sizeof VEC, args, 1, 0, 0,
                                         &result, vt);
    if (rc == DFP_ENOSYS || rc == DFP_ETRACE) {
        printf("# SKIP vec parity: live ptrace producer unavailable (rc=%d)\n",
               rc);
        asmtest_valtrace_free(vt);
        return;
    }
    CHECK(rc == DFP_OK || rc == DFP_FAULT, "ptrace vec _run rc=%d", rc);
    CHECK(vt->regfile != NULL, "live vec regfile not armed / filled");

    /* --- parity: xmm0 agrees once written (step 1 onward, base-INDEPENDENT) --- */
    size_t n = enh < vt->steps_len ? enh : vt->steps_len;
    size_t vec_compared = 0;
    for (size_t i = 1; i < n; i++) {
        uint64_t emu_off = emu[i].rip - EMU_CODE_BASE;
        if (emu_off != vt->insn_off[i])
            break; /* streams diverged: stop */
        if (vt->insn_off[i] >= sizeof VEC)
            break; /* stepped out of the routine's bytes */
        CHECK(vt->regfile[i].has_vec, "step %zu live has_vec not set", i);
        /* The MXCSR control/status word is never 0 on a live x86-64 process
         * (default 0x1f80), so a nonzero value proves the GETFPREGS deck was read. */
        CHECK(vt->regfile[i].mxcsr != 0, "step %zu live mxcsr unread (0)", i);
        int eq = memcmp(emu[i].xmm[0].u8, vt->regfile[i].xmm[0], 16) == 0;
        if (!eq) {
            fprintf(stderr, "  emu  xmm0=");
            for (int k = 0; k < 16; k++)
                fprintf(stderr, "%02x", emu[i].xmm[0].u8[k]);
            fprintf(stderr, "\n  live xmm0=");
            for (int k = 0; k < 16; k++)
                fprintf(stderr, "%02x", vt->regfile[i].xmm[0][k]);
            fprintf(stderr, " (off %llu)\n",
                    (unsigned long long)vt->insn_off[i]);
        }
        CHECK(eq, "step %zu xmm0 differs (emu f64=%g vs live)", i,
              emu[i].xmm[0].f64[0]);
        vec_compared++;
    }
    CHECK(
        vec_compared >= 1,
        "no aligned vec steps compared — the wide-deck parity proved nothing");
    asmtest_valtrace_free(vt);
    if (vec_compared)
        printf("test_regstate_parity: vec OK (%zu step(s); xmm0 agrees across "
               "producers, mxcsr read)\n",
               vec_compared);
}

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
        CHECK((long)res.regs.rax == expect,
              "emulator result rax=%lld, want %ld", (long long)res.regs.rax,
              expect);
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
        CHECK(emu[i].rdi == vt->regfile[i].rdi,
              "step %zu rdi: emu=%llu live=%llu", i,
              (unsigned long long)emu[i].rdi,
              (unsigned long long)vt->regfile[i].rdi);
        CHECK(emu[i].rsi == vt->regfile[i].rsi,
              "step %zu rsi: emu=%llu live=%llu", i,
              (unsigned long long)emu[i].rsi,
              (unsigned long long)vt->regfile[i].rsi);
        if (i >= 1)
            CHECK(emu[i].rax == vt->regfile[i].rax,
                  "step %zu rax: emu=%llu live=%llu", i,
                  (unsigned long long)emu[i].rax,
                  (unsigned long long)vt->regfile[i].rax);
        compared++;
    }
    CHECK(compared >= 4, "only %zu aligned steps compared (want >= 4)",
          compared);
    CHECK(rip_differs > 0,
          "rip never differed — the two captures share a basis, so the "
          "computation-register agreement is not a cross-basis result");

    asmtest_valtrace_free(vt);

    /* R4 T1: the wide FP/vector deck parity, over the same two producers. */
    vec_parity();

    if (fails) {
        fprintf(stderr, "test_regstate_parity: %d check(s) FAILED\n", fails);
        return 1;
    }
    printf(
        "test_regstate_parity: OK (%zu aligned steps; rax/rdi/rsi agree with "
        "the emulator modulo base, rip differs)\n",
        compared);
    return 0;
}
