/*
 * test_reweave.c — 30 R3 (resume-from-state seam + Reweave), T1 + T2.
 *
 * T1 (re-host is byte-identical): the emulator L0 value producer, re-hosted on an
 * emu_t engine (asmtest_dataflow_emu_run_hosted, on a fresh emu_open handle),
 * produces a valtrace BYTE-FOR-BYTE identical to the self-contained standalone
 * (asmtest_dataflow_emu_run) for every routine. The re-host is a pure refactor —
 * zero observable change — so it can carry the checkpoint/resume machinery T2/T3
 * need without moving the golden. This is the direct, targeted form of the
 * brief's "prove byte-identity first" (a stronger check than the serialized
 * golden, which only sees the final .asmtrace).
 *
 * T2 (checkpoint + resume): snapshot the guest at a chosen execution step K, then
 * emu_restore and re-run forward. The resumed trace is the TAIL of the original,
 * aligned by absolute step — resumed step j == original step K+j byte-for-byte,
 * because the restored register + memory state IS the original's pre-step-K state.
 * And a one-register edit at K (between restore and resume) produces a
 * counterfactual that diverges only downstream of the edit — the Reweave seam.
 *
 * x86-64 guest only (the corpus is host-arch assembly); the mk rule gates the
 * build on x86_64 + libunicorn, so a run here means both are present.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asmtest_emu.h"      /* emu_t, emu_open/close, emu_snapshot_t + free */
#include "asmtest_valtrace.h" /* asmtest_valtrace_t + at_val_rec_t            */

/* The producer tier ships no public header; its callers forward-declare it (the
 * tools/asmtrace_record.c + cli/test_mem_parity.c precedent). */
int asmtest_dataflow_emu_run(const uint8_t *code, size_t code_len,
                             const long *args, int nargs, uint64_t max_insns,
                             asmtest_valtrace_t *vt);
int asmtest_dataflow_emu_run_hosted(emu_t *e, const uint8_t *code,
                                    size_t code_len, const long *args, int nargs,
                                    uint64_t max_insns, asmtest_valtrace_t *vt);
int asmtest_dataflow_emu_checkpoint(emu_t *e, const uint8_t *code,
                                    size_t code_len, const long *args, int nargs,
                                    uint64_t max_insns, uint64_t k,
                                    asmtest_valtrace_t *vt,
                                    emu_snapshot_t **snap_out);
int asmtest_dataflow_emu_resume(emu_t *e, const emu_snapshot_t *snap,
                                const uint8_t *code, size_t code_len,
                                asmtest_valtrace_t *vt);
int asmtest_dataflow_emu_run_from_current(emu_t *e, const uint8_t *code,
                                          size_t code_len,
                                          asmtest_valtrace_t *vt,
                                          emu_result_t *result_out);
bool asmtest_dataflow_emu_edit_gp(emu_t *e, const char *name, uint64_t val);

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

/* Deep byte-for-byte equality of one operand record — every field the writer
 * serializes, so "equal here" means "equal in the .asmtrace". */
static int rec_eq(const at_val_rec_t *a, const at_val_rec_t *b) {
    return a->kind == b->kind && a->reg == b->reg && a->base == b->base &&
           a->index == b->index && a->scale == b->scale && a->disp == b->disp &&
           a->addr == b->addr && a->size == b->size &&
           a->is_write == b->is_write && a->value_valid == b->value_valid &&
           a->wide == b->wide && a->wide_off == b->wide_off &&
           a->value == b->value && a->step == b->step;
}

/* Assert two value traces are byte-identical: same step spine, same operand
 * records, same wide side buffer. `label` names the comparison in a failure. */
static void assert_vt_equal(const char *label, const asmtest_valtrace_t *a,
                            const asmtest_valtrace_t *b) {
    CHECK(a->steps_len == b->steps_len, "%s: steps_len %zu != %zu", label,
          a->steps_len, b->steps_len);
    CHECK(a->steps_total == b->steps_total, "%s: steps_total %llu != %llu",
          label, (unsigned long long)a->steps_total,
          (unsigned long long)b->steps_total);
    size_t ns = a->steps_len < b->steps_len ? a->steps_len : b->steps_len;
    for (size_t i = 0; i < ns; i++)
        CHECK(a->insn_off[i] == b->insn_off[i],
              "%s: insn_off[%zu] 0x%llx != 0x%llx", label, i,
              (unsigned long long)a->insn_off[i],
              (unsigned long long)b->insn_off[i]);
    CHECK(a->recs_len == b->recs_len, "%s: recs_len %zu != %zu", label,
          a->recs_len, b->recs_len);
    CHECK(a->recs_total == b->recs_total, "%s: recs_total %llu != %llu", label,
          (unsigned long long)a->recs_total, (unsigned long long)b->recs_total);
    size_t nr = a->recs_len < b->recs_len ? a->recs_len : b->recs_len;
    for (size_t i = 0; i < nr; i++)
        CHECK(rec_eq(&a->recs[i], &b->recs[i]),
              "%s: rec[%zu] differs (kind=%d/%d reg=%u/%u addr=0x%llx/0x%llx "
              "val=0x%llx/0x%llx step=%u/%u)",
              label, i, a->recs[i].kind, b->recs[i].kind, a->recs[i].reg,
              b->recs[i].reg, (unsigned long long)a->recs[i].addr,
              (unsigned long long)b->recs[i].addr,
              (unsigned long long)a->recs[i].value,
              (unsigned long long)b->recs[i].value, a->recs[i].step,
              b->recs[i].step);
    CHECK(a->wide_len == b->wide_len, "%s: wide_len %zu != %zu", label,
          a->wide_len, b->wide_len);
    size_t nw = a->wide_len < b->wide_len ? a->wide_len : b->wide_len;
    if (nw > 0)
        CHECK(memcmp(a->wide, b->wide, nw) == 0, "%s: wide[] bytes differ",
              label);
}

/* Index of the first record whose step >= k (records are step-ordered). */
static size_t first_rec_at_step(const asmtest_valtrace_t *vt, uint64_t k) {
    for (size_t i = 0; i < vt->recs_len; i++)
        if (vt->recs[i].step >= (uint32_t)k)
            return i;
    return vt->recs_len;
}

/* Assert `tail` (a resumed trace whose step 0 IS the original's step k) is
 * byte-for-byte the tail of `full` from absolute step k: same step count, same
 * per-step offsets, and same operand records — every field equal except the
 * absolute step number, which is shifted by k (full.step == tail.step + k). This
 * is the direct proof of T2's "the resume reproduces the tail byte-for-byte." */
static void assert_tail_identical(const char *label,
                                  const asmtest_valtrace_t *full, uint64_t k,
                                  const asmtest_valtrace_t *tail) {
    CHECK(full->steps_len - (size_t)k == tail->steps_len,
          "%s: tail steps_len %zu != full[%llu:] = %zu", label, tail->steps_len,
          (unsigned long long)k, full->steps_len - (size_t)k);
    size_t ns = tail->steps_len;
    if (full->steps_len - (size_t)k < ns)
        ns = full->steps_len - (size_t)k;
    for (size_t i = 0; i < ns; i++)
        CHECK(full->insn_off[(size_t)k + i] == tail->insn_off[i],
              "%s: tail insn_off[%zu] 0x%llx != full[%llu] 0x%llx", label, i,
              (unsigned long long)tail->insn_off[i],
              (unsigned long long)(k + i),
              (unsigned long long)full->insn_off[(size_t)k + i]);
    size_t fi = first_rec_at_step(full, k);
    size_t nrec_full = full->recs_len - fi;
    CHECK(nrec_full == tail->recs_len, "%s: tail recs_len %zu != full[%llu:] %zu",
          label, tail->recs_len, (unsigned long long)k, nrec_full);
    size_t nr = nrec_full < tail->recs_len ? nrec_full : tail->recs_len;
    for (size_t i = 0; i < nr; i++) {
        at_val_rec_t a = full->recs[fi + i]; /* copy; shift step for compare */
        a.step -= (uint32_t)k;
        CHECK(rec_eq(&a, &tail->recs[i]),
              "%s: tail rec[%zu] differs from full (step %u vs %u, val "
              "0x%llx vs 0x%llx, addr 0x%llx vs 0x%llx)",
              label, i, a.step, tail->recs[i].step,
              (unsigned long long)a.value,
              (unsigned long long)tail->recs[i].value,
              (unsigned long long)a.addr,
              (unsigned long long)tail->recs[i].addr);
    }
}

/* Count records whose value differs between two same-shape traces (the tail of a
 * clean resume vs an edited resume). >0 proves the counterfactual diverged. */
static size_t value_divergences(const asmtest_valtrace_t *a,
                                const asmtest_valtrace_t *b) {
    size_t n = a->recs_len < b->recs_len ? a->recs_len : b->recs_len, d = 0;
    for (size_t i = 0; i < n; i++)
        if (a->recs[i].value_valid && b->recs[i].value_valid &&
            a->recs[i].value != b->recs[i].value)
            d++;
    return d;
}

int main(void) {
    /* poly(a, b, c): a straight-line routine long enough for a mid-run
     * checkpoint, mixing GP arithmetic with a stack spill/reload so the snapshot
     * has to carry both register and memory state.
     *   step 0  mov rax, rdi        48 89 f8
     *   step 1  add rax, rsi        48 01 f0
     *   step 2  add rax, rdx        48 01 d0
     *   step 3  imul rax, rax       48 0f af c0   <-- checkpoint K here
     *   step 4  mov [rsp-8], rax    48 89 44 24 f8
     *   step 5  mov rcx, [rsp-8]    48 8b 4c 24 f8
     *   step 6  add rax, rcx        48 01 c8
     *   step 7  ret                 c3
     * poly(2,3,4) = let s=2+3+4=9; s*s=81; +81 = 162. */
    static const uint8_t POLY[] = {
        0x48, 0x89, 0xf8,             /* mov rax, rdi     */
        0x48, 0x01, 0xf0,             /* add rax, rsi     */
        0x48, 0x01, 0xd0,             /* add rax, rdx     */
        0x48, 0x0f, 0xaf, 0xc0,       /* imul rax, rax    */
        0x48, 0x89, 0x44, 0x24, 0xf8, /* mov [rsp-8], rax */
        0x48, 0x8b, 0x4c, 0x24, 0xf8, /* mov rcx, [rsp-8] */
        0x48, 0x01, 0xc8,             /* add rax, rcx     */
        0xc3,                         /* ret              */
    };
    const long args[3] = {2, 3, 4};

    /* store_load(a, b): the mem-exercising routine from test_mem_parity, to widen
     * T1's byte-identity check to memory records. */
    static const uint8_t STORE_LOAD[] = {
        0x48, 0x89, 0x7c, 0x24, 0xf8, /* mov [rsp-8], rdi */
        0x48, 0x8b, 0x44, 0x24, 0xf8, /* mov rax, [rsp-8] */
        0x48, 0x01, 0xf0,             /* add rax, rsi     */
        0xc3,                         /* ret              */
    };
    const long args2[2] = {6, 7};

    /* ============================================================= *
     * T1 — the emu_t-hosted run is byte-identical to the standalone  *
     * ============================================================= */
    struct {
        const char *name;
        const uint8_t *code;
        size_t len;
        const long *args;
        int nargs;
    } cases[] = {
        {"poly", POLY, sizeof POLY, args, 3},
        {"store_load", STORE_LOAD, sizeof STORE_LOAD, args2, 2},
    };
    for (size_t ci = 0; ci < sizeof cases / sizeof cases[0]; ci++) {
        asmtest_valtrace_t *vs = asmtest_valtrace_new(64, 512, 512);
        asmtest_valtrace_t *vh = asmtest_valtrace_new(64, 512, 512);
        CHECK(vs && vh, "valtrace_new failed for %s", cases[ci].name);
        if (!vs || !vh)
            return 1;
        int rcs = asmtest_dataflow_emu_run(cases[ci].code, cases[ci].len,
                                           cases[ci].args, cases[ci].nargs, 0,
                                           vs);
        emu_t *e = emu_open();
        CHECK(e != NULL, "emu_open failed");
        if (!e)
            return 1;
        int rch = asmtest_dataflow_emu_run_hosted(e, cases[ci].code,
                                                  cases[ci].len, cases[ci].args,
                                                  cases[ci].nargs, 0, vh);
        emu_close(e);
        CHECK(rcs == rch, "%s: standalone rc=%d != hosted rc=%d", cases[ci].name,
              rcs, rch);
        CHECK(rcs == 0, "%s: standalone did not run clean (rc=%d)",
              cases[ci].name, rcs);
        assert_vt_equal(cases[ci].name, vs, vh);
        CHECK(vs->steps_len > 0, "%s: empty trace", cases[ci].name);
        asmtest_valtrace_free(vs);
        asmtest_valtrace_free(vh);
    }

    /* ============================================================= *
     * T2 — checkpoint at step K, resume reproduces the tail exactly *
     * ============================================================= */
    const uint64_t K = 3; /* pre-imul: rax holds a+b+c = 9 */

    /* The reference full run (standalone). */
    asmtest_valtrace_t *full = asmtest_valtrace_new(64, 512, 512);
    CHECK(full != NULL, "full valtrace_new failed");
    if (!full)
        return 1;
    int rcf = asmtest_dataflow_emu_run(POLY, sizeof POLY, args, 3, 0, full);
    CHECK(rcf == 0, "full run rc=%d", rcf);
    CHECK(full->steps_len == 8, "poly should be 8 steps, got %zu",
          full->steps_len);

    /* Checkpoint at K on a fresh handle, capturing the snapshot AND the full
     * trace (which must equal the reference — checkpoint is a superset of the
     * hosted run). */
    emu_t *e = emu_open();
    CHECK(e != NULL, "emu_open (checkpoint) failed");
    if (!e)
        return 1;
    asmtest_valtrace_t *cvt = asmtest_valtrace_new(64, 512, 512);
    emu_snapshot_t *snap = NULL;
    int rcc = asmtest_dataflow_emu_checkpoint(e, POLY, sizeof POLY, args, 3, 0,
                                              K, cvt, &snap);
    CHECK(rcc == 0, "checkpoint rc=%d", rcc);
    CHECK(snap != NULL, "checkpoint produced no snapshot at K=%llu",
          (unsigned long long)K);
    assert_vt_equal("checkpoint-full", full, cvt);

    /* Resume (unedited): the tail must reproduce full[K:] byte-for-byte. */
    asmtest_valtrace_t *tail = asmtest_valtrace_new(64, 512, 512);
    CHECK(tail != NULL, "tail valtrace_new failed");
    int rcr = asmtest_dataflow_emu_resume(e, snap, POLY, sizeof POLY, tail);
    CHECK(rcr == 0, "resume rc=%d", rcr);
    assert_tail_identical("resume-tail", full, K, tail);

    /* ============================================================= *
     * T2 — Reweave: an edit at K diverges only downstream           *
     * ============================================================= */
    asmtest_valtrace_t *edited = asmtest_valtrace_new(64, 512, 512);
    CHECK(edited != NULL, "edited valtrace_new failed");
    /* Restore to pre-K, poke rax := 0 (was a+b+c = 9), then run the SAME tail via
     * run_from_current. The instruction spine is unchanged (straight-line code, no
     * value-dependent branch), so offsets + step count match the clean tail — the
     * divergence is purely in the computed VALUES (imul(0)=0, +0 = 0). emu_restore
     * is declared in asmtest_emu.h. */
    CHECK(emu_restore(e, snap), "edited: emu_restore failed");
    CHECK(asmtest_dataflow_emu_edit_gp(e, "rax", 0),
          "edited: edit_gp(rax) failed");
    emu_result_t edited_result = {0};
    int rce = asmtest_dataflow_emu_run_from_current(e, POLY, sizeof POLY,
                                                    edited, &edited_result);
    CHECK(rce == 0, "edited resume rc=%d", rce);
    CHECK(!edited_result.faulted,
          "edited (rax:=0) resume should not fault (straight-line code, only "
          "values diverge)");
    /* Same instruction spine as the clean tail (offsets + step count unchanged). */
    CHECK(edited->steps_len == tail->steps_len,
          "edited tail steps_len %zu != clean tail %zu (control flow moved?)",
          edited->steps_len, tail->steps_len);
    for (size_t i = 0; i < edited->steps_len && i < tail->steps_len; i++)
        CHECK(edited->insn_off[i] == tail->insn_off[i],
              "edited tail diverged in CONTROL FLOW at step %zu (off 0x%llx vs "
              "0x%llx) — expected divergence only in values",
              i, (unsigned long long)edited->insn_off[i],
              (unsigned long long)tail->insn_off[i]);
    /* ...but the VALUES diverge downstream of the edit. */
    size_t nd = value_divergences(tail, edited);
    CHECK(nd > 0,
          "edit at K (rax:=0) changed no record value — the counterfactual is "
          "not diverging");

    /* ============================================================= *
     * T3 — a genuine post-edit fault carries a rich emu_result_t    *
     * ============================================================= *
     * Reweave rsp to a non-canonical address right before poly's step-4
     * stack write ("mov [rsp-8], rax") reliably faults the guest (the same
     * trick desktop/test/test_loom_reweave.cpp uses). Reuses the SAME
     * checkpoint/snapshot (e/snap) T2 already set up above; e/snap are only
     * closed/freed after this block, at the end of main(). */
    CHECK(emu_restore(e, snap), "fault case: emu_restore failed");
    CHECK(asmtest_dataflow_emu_edit_gp(e, "rsp", 0x8000000000000000ULL),
          "fault case: edit_gp(rsp) failed");
    asmtest_valtrace_t *faulted_tail = asmtest_valtrace_new(64, 512, 512);
    CHECK(faulted_tail != NULL, "faulted_tail valtrace_new failed");
    emu_result_t fault_result = {0};
    int rcflt = asmtest_dataflow_emu_run_from_current(
        e, POLY, sizeof POLY, faulted_tail, &fault_result);
    CHECK(rcflt == 1,
          "fault case: expected a guest fault (rc==1), got rc=%d", rcflt);
    CHECK(fault_result.faulted,
          "fault case: emu_result_t.faulted should be true");
    CHECK(fault_result.fault_kind != EMU_FAULT_NONE,
          "fault case: emu_result_t.fault_kind should not be EMU_FAULT_NONE");
    CHECK(fault_result.regs.rip != 0,
          "fault case: emu_result_t.regs.rip should be nonzero — proves the "
          "register file was genuinely read back, not left zeroed");
    asmtest_valtrace_free(faulted_tail);

    emu_snapshot_free(snap);
    asmtest_valtrace_free(full);
    asmtest_valtrace_free(cvt);
    asmtest_valtrace_free(tail);
    asmtest_valtrace_free(edited);
    emu_close(e);

    if (fails) {
        fprintf(stderr, "test_reweave: %d check(s) FAILED\n", fails);
        return 1;
    }
    printf("test_reweave: OK (hosted run byte-identical to standalone; "
           "checkpoint@K=%llu resume tail identical; edit-at-K diverges "
           "downstream)\n",
           (unsigned long long)K);
    return 0;
}
