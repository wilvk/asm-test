/*
 * test_mem_parity.c — 29 R2 T3, the cross-producer correctness check for the
 * `mem` address stream.
 *
 * Run ONE deterministic load/store routine through BOTH memory-access producers:
 *   - the trusted EMULATOR L0 producer (asmtest_dataflow_emu_run, the `--mem`
 *     golden path), whose guest stack sits at the fixed DF_STACK_BASE, and
 *   - the LIVE single-step PTRACE producer (asmtest_dataflow_ptrace_run) running
 *     in a real forked tracee at an ASLR'd stack,
 * and assert the two per-access `mem` streams AGREE on the base-INDEPENDENT
 * structure while the effective addresses themselves DIFFER by construction (the
 * emulator's fixed stack base vs the tracee's real ASLR'd rsp). The absolute EAs
 * are meaningful only within one capture's address space; the schema's `space`
 * normalization is what a reader folds them by, never a raw cross-recording
 * compare.
 *
 * The two producers capture memory by DIFFERENT mechanisms, and the parity is
 * exactly the defensible one: the emulator uses Unicorn's hardware memory hooks,
 * so it sees EVERY access including implicit stack traffic (the `ret` pop of the
 * return address); the live producer enumerates Capstone operands, so it records
 * only EXPLICITLY-encoded memory operands. The live stream is therefore a SUBSET
 * of the emulator's: every live access must match an emulator access on
 * (step, size, rw), the emulator may carry additional implicit accesses the
 * operand enumerator never surfaces, and at least one matched pair's EA must
 * differ — proving the agreement is a genuine cross-basis result, not two
 * identical captures. (This mirrors the regstate parity's base-dependent-registers
 * split: same value semantics, different capture basis.)
 *
 * This is the parity the 3D rich rung relies on to render a live capture the same
 * way it does an emulator `--mem` recording: the live stream must carry the SAME
 * access structure as the trusted emulator stream, differing only by base.
 *
 * x86-64-guest only (the ptrace producer single-steps x86-64; the emulator corpus
 * is host-arch too). The mk rule gates the build on x86_64 + libunicorn; a run-time
 * ptrace refusal (seccomp) or an off-arch producer self-skips honestly.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asmtest_valtrace.h" /* asmtest_valtrace_t + at_val_rec_t */

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

/* One projected memory access, the `mem` event's own fields (step, ea, size, rw). */
typedef struct {
    uint32_t step;
    uint64_t ea;
    uint16_t size;
    int is_write;
} mem_ev;

/* Project a valtrace's memory-kind operand records into the `mem` stream, exactly
 * as the producers' emit paths do (emit_mem_stream / dataflow_record). Returns the
 * count; fills up to `cap`. */
static size_t collect_mem(const asmtest_valtrace_t *vt, mem_ev *out,
                          size_t cap) {
    size_t n = 0;
    for (size_t i = 0; i < vt->recs_len && n < cap; i++) {
        const at_val_rec_t *r = &vt->recs[i];
        if (r->kind != AT_LOC_MEM_ABS && r->kind != AT_LOC_MEM_OFF)
            continue;
        out[n].step = r->step;
        out[n].ea = r->addr;
        out[n].size = r->size;
        out[n].is_write = r->is_write ? 1 : 0;
        n++;
    }
    return n;
}

int main(void) {
    /* store_load(a, b): spill a to the stack, read it back, add b. One 8-byte
     * write then one 8-byte read at the SAME slot — two memory accesses whose
     * effective address is the stack, so it is base-dependent (differs across the
     * two producers) while the access STRUCTURE (step/size/rw) is not.
     *   0x00 mov [rsp-8], rdi   48 89 7c 24 f8
     *   0x05 mov rax, [rsp-8]   48 8b 44 24 f8
     *   0x0a add rax, rsi       48 01 f0
     *   0x0d ret                c3                                            */
    static const uint8_t ROUTINE[] = {
        0x48, 0x89, 0x7c, 0x24, 0xf8, /* mov [rsp-8], rdi */
        0x48, 0x8b, 0x44, 0x24, 0xf8, /* mov rax, [rsp-8] */
        0x48, 0x01, 0xf0,             /* add rax, rsi     */
        0xc3,                         /* ret              */
    };
    const long args[2] = {6, 7};
    const long expect = 6L + 7L; /* rax = a (reloaded) + b = 13 */

    mem_ev emu[16], live[16];
    size_t ne = 0, nl = 0;
    int emu_matched[16] = {0};

    /* --- the trusted emulator producer --------------------------------- */
    {
        asmtest_valtrace_t *vt = asmtest_valtrace_new(64, 512, 512);
        CHECK(vt != NULL, "emu valtrace_new failed");
        if (vt == NULL)
            return 1;
        int rc =
            asmtest_dataflow_emu_run(ROUTINE, sizeof ROUTINE, args, 2, 0, vt);
        CHECK(rc == 0 || rc == 1, "emu _run rc=%d", rc);
        ne = collect_mem(vt, emu, 16);
        asmtest_valtrace_free(vt);
    }
    /* Store + load are both explicit; the emulator's hooks additionally catch the
     * `ret` pop, so it sees at least those three. */
    CHECK(ne >= 2, "emulator saw only %zu memory accesses, want >= 2", ne);

    /* --- the live ptrace producer -------------------------------------- */
    asmtest_valtrace_t *vt = asmtest_valtrace_new(64, 512, 512);
    CHECK(vt != NULL, "live valtrace_new failed");
    if (vt == NULL)
        return 1;
    long result = 0;
    int rc = asmtest_dataflow_ptrace_run(ROUTINE, sizeof ROUTINE, args, 2, 0, 0,
                                         &result, vt);
    if (rc == DFP_ENOSYS || rc == DFP_ETRACE) {
        printf("# SKIP test_mem_parity: live ptrace producer unavailable "
               "(rc=%d: off x86-64 / no Capstone / ptrace refused)\n",
               rc);
        asmtest_valtrace_free(vt);
        return 0;
    }
    CHECK(rc == DFP_OK || rc == DFP_FAULT, "ptrace _run rc=%d", rc);
    CHECK(result == expect, "live result=%ld, want %ld", result, expect);
    nl = collect_mem(vt, live, 16);
    asmtest_valtrace_free(vt);
    /* The two explicit accesses (the store then the load) — exactly what the
     * operand enumerator surfaces. */
    CHECK(nl == 2, "live saw %zu memory accesses, want 2 (explicit store+load)",
          nl);
    CHECK(nl >= 1 && live[0].is_write && !live[1].is_write,
          "expected live access[0]=write (store), access[1]=read (load)");

    /* --- parity: the live stream is a structural subset of the emulator's ---- */
    CHECK(ne >= nl,
          "emulator (%zu) should see at least as many accesses as the "
          "operand-only live stream (%zu)",
          ne, nl);
    size_t ea_differs = 0;
    for (size_t i = 0; i < nl; i++) {
        int found = -1;
        for (size_t j = 0; j < ne; j++) {
            if (!emu_matched[j] && emu[j].step == live[i].step &&
                emu[j].size == live[i].size &&
                emu[j].is_write == live[i].is_write) {
                found = (int)j;
                emu_matched[j] = 1;
                break;
            }
        }
        CHECK(
            found >= 0,
            "live access %zu (step=%u size=%u rw=%d) has no matching emulator "
            "access on (step,size,rw)",
            i, live[i].step, live[i].size, live[i].is_write);
        if (found >= 0 && emu[found].ea != live[i].ea)
            ea_differs++;
    }
    /* Base-dependent: the two captures use different stack bases, so at least one
     * matched pair's EA must differ — proving the structural agreement is a genuine
     * cross-basis result and not two identical captures. */
    CHECK(ea_differs > 0,
          "effective addresses never differed — the two captures share a stack "
          "base, so the structural agreement is not a cross-basis result");

    if (fails) {
        fprintf(stderr, "test_mem_parity: %d check(s) FAILED\n", fails);
        return 1;
    }
    printf("test_mem_parity: OK (%zu live accesses subset of %zu emulator; "
           "step/size/rw agree, effective addresses differ by base)\n",
           nl, ne);
    return 0;
}
