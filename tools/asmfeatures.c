/*
 * asmfeatures.c — the feature-benchmark probe (Phase 1).
 *
 * A live capability sweep of THIS system: which trace tiers/backends are
 * available, why the absent ones self-skip, and — the completeness metric —
 * whether each exercised backend actually produces a COMPLETE (non-truncated)
 * trace here. It emits the trace-parity matrices instantiated per system
 * (docs/internal/analysis/trace-parity-matrix.md) as JSON, using only existing
 * public APIs, and NEVER fails on an absent capability: an unavailable tier is
 * recorded as data (available:false + skip_reason), not an error.
 *
 *   ./asmfeatures                 -> {"features":[ ... ]}  (default)
 *   ./asmfeatures --format=text   -> human-readable table
 *
 * Completeness is measured two ways:
 *   - emulator guests: run a representative routine traced, report !truncated
 *     (exact by construction — the universal floor's completeness reference);
 *   - host-native capture ladder (x86-64 Linux/macOS): run a straight-line routine
 *     AND a sweep of counted loops of growing trip count, both under the box's raw
 *     TOP hardware backend (`native-hw` — the fixed-window ceiling is VISIBLE) and
 *     under the auto-escalating tier (`native-auto` — the ceiling is RESTORED),
 *     reporting captured insns vs the emulator's deterministic truth. captured <
 *     truth is precisely how one box's hardware (AMD LBR) truncates where another's
 *     (Intel PT / single-step) does not — the cross-box capture metric.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "asmbench_fixtures.h"
#include "asmtest_drtrace.h"
#include "asmtest_emu.h"
#include "asmtest_hwtrace.h"
#include "asmtest_trace.h"
#include "asmtest_trace_auto.h"

/* The host-native capture ladder runs the real bytes in-process, so it needs the
 * single-step / Intel-PT / AMD-LBR path — today x86-64 Linux OR macOS (single-step
 * is the macOS floor; exec_alloc is W^X-portable to Darwin). Elsewhere the ladder
 * self-skips to an explicit unavailable row (absent = data, not an invisible gap). */
#if defined(__x86_64__) && (defined(__linux__) || defined(__APPLE__))
#define ASMFEATURES_HOST_CAPTURE 1
#endif

/* Print a JSON string value with the minimal escaping skip_reason needs. */
static void json_str(const char *s) {
    putchar('"');
    for (; s && *s; s++) {
        if (*s == '"' || *s == '\\')
            putchar('\\');
        if (*s == '\n') {
            fputs("\\n", stdout);
            continue;
        }
        putchar(*s);
    }
    putchar('"');
}

/* One feature row. `complete`/`insns`/`insns_truth` are -1 when not measured
 * (printed null). `insns_truth` is the host-independent deterministic instruction
 * count (from the emulator) that a capture row's `trace_insns` is measured against
 * — captured < truth is exactly how a fixed-window hardware backend truncates. */
static void row(int first, const char *tier, const char *backend,
                const char *arch, const char *scope, int available,
                const char *skip_reason, const char *fidelity, int complete,
                long long insns, long long insns_truth, const char *note) {
    printf("%s    {", first ? "" : ",\n");
    printf("\"tier\": ");
    json_str(tier);
    printf(", \"backend\": ");
    json_str(backend);
    printf(", \"arch\": ");
    json_str(arch);
    printf(", \"scope\": ");
    json_str(scope);
    printf(", \"available\": %s", available ? "true" : "false");
    printf(", \"skip_reason\": ");
    json_str(skip_reason ? skip_reason : "");
    printf(", \"fidelity\": ");
    json_str(fidelity);
    if (complete < 0)
        printf(", \"complete\": null");
    else
        printf(", \"complete\": %s", complete ? "true" : "false");
    if (insns < 0)
        printf(", \"trace_insns\": null");
    else
        printf(", \"trace_insns\": %lld", insns);
    if (insns_truth < 0)
        printf(", \"insns_truth\": null");
    else
        printf(", \"insns_truth\": %lld", insns_truth);
    if (note) {
        printf(", \"note\": ");
        json_str(note);
    }
    printf("}");
}

/* Run a guest add3 traced; report available + completeness + insns. */
static void emu_guest_row(int first, const char *arch, int guest) {
    asmtest_trace_t *t = asmtest_trace_new(0, 64);
    int ok = 0, complete = 1;
    long long insns = -1;
    long a[3] = {2, 3, 4};
    if (t) {
        switch (guest) {
        case 0: { /* x86-64 */
            emu_t *e = emu_open();
            emu_result_t r;
            ok = e && emu_call_traced(e, FIX_X86_ADD3, sizeof FIX_X86_ADD3, a,
                                      3, 0, &r, t);
            if (e)
                emu_close(e);
            break;
        }
        case 1: { /* win64 */
            emu_t *e = emu_open();
            emu_result_t r;
            ok = e &&
                 emu_call_win64_traced(e, FIX_WIN64_ADD3, sizeof FIX_WIN64_ADD3,
                                       a, 3, 0, &r, t);
            if (e)
                emu_close(e);
            break;
        }
        case 2: { /* arm64 */
            emu_arm64_t *e = emu_arm64_open();
            emu_arm64_result_t r;
            ok = e && emu_arm64_call_traced(
                          e, FIX_A64_ADD3, sizeof FIX_A64_ADD3, a, 3, 0, &r, t);
            if (e)
                emu_arm64_close(e);
            break;
        }
        case 3: { /* riscv64 */
            emu_riscv_t *e = emu_riscv_open();
            emu_riscv_result_t r;
            ok = e && emu_riscv_call_traced(e, FIX_RV_ADD3, sizeof FIX_RV_ADD3,
                                            a, 3, 0, &r, t);
            if (e)
                emu_riscv_close(e);
            break;
        }
        case 4: { /* arm32 */
            emu_arm_t *e = emu_arm_open();
            emu_arm_result_t r;
            ok = e && emu_arm_call_traced(e, FIX_A32_ADD3, sizeof FIX_A32_ADD3,
                                          a, 3, 0, &r, t);
            if (e)
                emu_arm_close(e);
            break;
        }
        }
        insns = (long long)asmtest_emu_trace_insns_total(t);
        complete = !asmtest_emu_trace_truncated(t);
        asmtest_trace_free(t);
    }
    row(first, "emulator", "guest", arch, "guest", ok,
        ok ? "" : "emu run failed", "virtual-exact", ok ? complete : -1,
        ok ? insns : -1, -1, NULL);
}

static const char *hw_backend_name(asmtest_trace_backend_t b) {
    switch (b) {
    case ASMTEST_HWTRACE_INTEL_PT:
        return "intel_pt";
    case ASMTEST_HWTRACE_CORESIGHT:
        return "coresight";
    case ASMTEST_HWTRACE_AMD_LBR:
        return "amd_lbr";
    case ASMTEST_HWTRACE_SINGLESTEP:
        return "single_step";
    }
    return "native";
}

static const char *tier_name(asmtest_trace_tier_t t) {
    switch (t) {
    case ASMTEST_TIER_HWTRACE:
        return "hwtrace";
    case ASMTEST_TIER_DYNAMORIO:
        return "dynamorio";
    case ASMTEST_TIER_EMULATOR:
        return "emulator";
    }
    return "native";
}

#ifdef ASMFEATURES_HOST_CAPTURE
/* Host-independent ground truth for a host x86-64 fixture: run it under the exact
 * emulator and return the deterministic instruction count. -1 on failure. This is
 * the reference a native capture row's trace_insns is measured against — captured
 * < truth is precisely how a fixed-window hardware backend (AMD LBR) truncates. */
static long long emu_x86_insns(const unsigned char *bytes, size_t len,
                               const long *args, int nargs) {
    asmtest_trace_t *t = asmtest_trace_new(8192, 512);
    if (!t)
        return -1;
    emu_t *e = emu_open();
    emu_result_t r;
    long long insns = -1;
    if (e && emu_call_traced(e, bytes, len, args, nargs, 0, &r, t))
        insns = (long long)asmtest_emu_trace_insns_total(t);
    if (e)
        emu_close(e);
    asmtest_trace_free(t);
    return insns;
}

/* Invoke code(args…) as a SysV routine with up to 6 int args (a shorter callee
 * harmlessly ignores the extra register args) — the in-process call the native
 * capture probe brackets with a trace begin/end. */
static long native_invoke(const void *code, const long *args, int nargs) {
    long a[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < nargs && i < 6; i++)
        a[i] = args[i];
    long (*fn)(long, long, long, long, long, long) =
        (long (*)(long, long, long, long, long, long))(uintptr_t)code;
    return fn(a[0], a[1], a[2], a[3], a[4], a[5]);
}

/* Trace one ladder rung through the box's TOP hardware backend RAW — no
 * escalation — so a fixed-window backend's truncation is observed, not hidden.
 * `backend` is the asmtest_hwtrace_auto() pick; returns captured insns (>=0) and
 * sets *complete, or -1 if the backend could not trace this rung. */
static long long native_hw_capture(int backend, const unsigned char *bytes,
                                   size_t len, const long *args, int nargs,
                                   int *complete) {
    void *exec = NULL;
    size_t exec_len = 0;
    if (asmtest_hwtrace_exec_alloc(bytes, len, &exec, &exec_len) !=
        ASMTEST_HW_OK)
        return -1;
    asmtest_trace_t *t = asmtest_trace_new(8192, 512);
    long long insns = -1;
    if (t) {
        asmtest_hwtrace_options_t opts;
        memset(&opts, 0, sizeof opts);
        opts.struct_size = sizeof opts; /* self-describe (flag-day ABI guard) */
        opts.backend = (asmtest_trace_backend_t)backend;
        if (asmtest_hwtrace_init(&opts) == ASMTEST_HW_OK) {
            if (asmtest_hwtrace_register_region("cap", exec, len, t) ==
                ASMTEST_HW_OK) {
                asmtest_hwtrace_begin("cap");
                (void)native_invoke(exec, args, nargs);
                asmtest_hwtrace_end("cap");
                insns = (long long)asmtest_emu_trace_insns_total(t);
                if (complete)
                    *complete = !asmtest_emu_trace_truncated(t);
            }
            asmtest_hwtrace_shutdown();
        }
        asmtest_trace_free(t);
    }
    asmtest_hwtrace_exec_free(exec, exec_len);
    return insns;
}
#endif /* ASMFEATURES_HOST_CAPTURE */

int main(int argc, char **argv) {
    int json = 1;
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--format=text") == 0)
            json = 0;

    if (!json) {
        /* Text mode: reuse the JSON path's data but print a compact table by
         * re-emitting through a tiny shim is overkill; keep text minimal. */
        printf("# asmfeatures — run with default (JSON) for the full report\n");
    }
    printf("{\"features\": [\n");
    int first = 1;

    /* 1) Emulator guests — the universal floor; completeness is the reference. */
    const char *guests[] = {"x86_64", "win64", "arm64", "riscv64", "arm32"};
    for (int g = 0; g < 5; g++) {
        emu_guest_row(first, guests[g], g);
        first = 0;
    }

    /* 2) Host-native instruction-CAPTURE depth — the hardware-feature metric that
     * varies across boxes. A straight-line routine plus a ladder of counted loops
     * of growing trip count, each run under the box's TOP hardware backend (raw,
     * `native-hw`) AND under the auto-escalating tier (`native-auto`). Captured
     * insns are compared to the emulator's deterministic truth: an UNBOUNDED backend
     * (Intel PT, single-step) captures every rung whole, so captured==truth; a
     * FIXED-WINDOW backend (AMD LBR, 16-deep) reconstructs only its last window, so
     * its raw capture PLATEAUS while the truth climbs — that divergence is the box's
     * hardware signature (Zen vs Intel vs Apple). The auto tier then ESCALATES past
     * the ceiling to a complete floor, so `native-auto` reports what the box
     * ultimately captures + which backend won. The native tiers EXECUTE the real
     * bytes, so each fixture is materialized into W^X memory first. */
#ifdef ASMFEATURES_HOST_CAPTURE
    {
        /* Two orthogonal capture axes. LOOP: insns(sum_to_n) = 3n+2, branches = n
         * — sweeps branch density; trip counts straddle the 16-deep AMD-LBR window
         * (4 within, 16 at the edge, 64 and 200 well over). RECURSION: insns(tri) =
         * 8n+4, call/ret nested n deep — sweeps call-STACK depth (LBR's 16-deep
         * return stack), which a loop never overflows. Both diverge from truth
         * exactly where the box's hardware ceiling is, but at different limits. */
        static const struct {
            const char *workload;
            const unsigned char *bytes;
            size_t len;
            long arg;
            int nargs;
        } ladder[] = {
            {"math.add3", FIX_X86_ADD3, sizeof FIX_X86_ADD3, 0, 3},
            {"loop.sum_to_4", FIX_X86_SUMTON, sizeof FIX_X86_SUMTON, 4, 1},
            {"loop.sum_to_16", FIX_X86_SUMTON, sizeof FIX_X86_SUMTON, 16, 1},
            {"loop.sum_to_64", FIX_X86_SUMTON, sizeof FIX_X86_SUMTON, 64, 1},
            {"loop.sum_to_200", FIX_X86_SUMTON, sizeof FIX_X86_SUMTON, 200, 1},
            {"recurse.tri_4", FIX_X86_TRI, sizeof FIX_X86_TRI, 4, 1},
            {"recurse.tri_16", FIX_X86_TRI, sizeof FIX_X86_TRI, 16, 1},
            {"recurse.tri_32", FIX_X86_TRI, sizeof FIX_X86_TRI, 32, 1},
        };
        long a3[3] = {2, 3, 4};
        int top_hw =
            asmtest_hwtrace_auto(ASMTEST_HWTRACE_BEST); /* -1 if none */
        for (size_t p = 0; p < sizeof ladder / sizeof ladder[0]; p++) {
            const long *args = (ladder[p].nargs == 3) ? a3 : &ladder[p].arg;
            long long truth = emu_x86_insns(ladder[p].bytes, ladder[p].len,
                                            args, ladder[p].nargs);

            /* (a) Raw top hardware backend — the ceiling is VISIBLE here. */
            if (top_hw >= 0) {
                int complete = 1;
                long long cap =
                    native_hw_capture(top_hw, ladder[p].bytes, ladder[p].len,
                                      args, ladder[p].nargs, &complete);
                if (cap >= 0)
                    row(first, "native-hw", hw_backend_name(top_hw), "x86_64",
                        "host", 1, "", "native", complete, cap, truth,
                        ladder[p].workload);
                else
                    row(first, "native-hw", hw_backend_name(top_hw), "x86_64",
                        "host", 0, "backend could not trace rung", "native", -1,
                        -1, truth, ladder[p].workload);
                first = 0;
            }

            /* (b) Auto-escalating tier — the ceiling is RESTORED here (the point of
             * the auto tier); reports the winning backend + final completeness. */
            void *exec = NULL;
            size_t exec_len = 0;
            if (asmtest_hwtrace_exec_alloc(ladder[p].bytes, ladder[p].len,
                                           &exec, &exec_len) != ASMTEST_HW_OK) {
                row(first, "native-auto", "native", "x86_64", "host", 0,
                    "exec_alloc failed", "native", -1, -1, truth,
                    ladder[p].workload);
                first = 0;
                continue;
            }
            asmtest_trace_t *t = asmtest_trace_new(8192, 512);
            asmtest_trace_choice_t used;
            memset(&used, 0, sizeof used);
            long result = 0;
            int rc = asmtest_trace_call_auto(
                exec, ladder[p].len, args, ladder[p].nargs, ASMTEST_TRACE_BEST,
                &result, t, &used);
            if (rc == ASMTEST_HW_OK) {
                const char *bk = (used.tier == ASMTEST_TIER_HWTRACE)
                                     ? hw_backend_name(used.backend)
                                     : tier_name(used.tier);
                long long insns = (long long)asmtest_emu_trace_insns_total(t);
                int complete = !asmtest_emu_trace_truncated(t);
                row(first, "native-auto", bk, "x86_64", "host", 1, "", "native",
                    complete, insns, truth, ladder[p].workload);
            } else {
                row(first, "native-auto", "native", "x86_64", "host", 0,
                    "no call-owning native tier available", "native", -1, -1,
                    truth, ladder[p].workload);
            }
            first = 0;
            asmtest_trace_free(t);
            asmtest_hwtrace_exec_free(exec, exec_len);
        }
    }
#else
    /* Absent = data, not an invisible gap: emit explicit unavailable rows so the
     * capability still appears in the cross-system matrix (matches how every other
     * tier self-skips with a reason). */
    row(first, "native-hw", "native", "host", "host", 0,
        "host-native capture ladder: x86-64 Linux/macOS only", "native", -1, -1,
        -1, NULL);
    first = 0;
    row(first, "native-auto", "native", "host", "host", 0,
        "host-native capture ladder: x86-64 Linux/macOS only", "native", -1, -1,
        -1, NULL);
    first = 0;
#endif

    /* 3) Static hardware-backend availability (no run — the static capability). */
    asmtest_trace_backend_t hw[] = {
        ASMTEST_HWTRACE_INTEL_PT, ASMTEST_HWTRACE_AMD_LBR,
        ASMTEST_HWTRACE_SINGLESTEP, ASMTEST_HWTRACE_CORESIGHT};
    for (int i = 0; i < 4; i++) {
        int avail = asmtest_hwtrace_available(hw[i]);
        char reason[256] = "";
        asmtest_hwtrace_skip_reason(hw[i], reason, sizeof reason);
        row(first, "hwtrace", hw_backend_name(hw[i]), "host", "host", avail,
            avail ? "" : reason, "native", -1, -1, -1, NULL);
        first = 0;
    }

    /* 4) DynamoRIO tier. */
    {
        int avail = asmtest_dr_available();
        char reason[256] = "";
        asmtest_dr_skip_reason(reason, sizeof reason);
        row(first, "dynamorio", "dynamorio", "host", "host", avail,
            avail ? "" : reason, "native", -1, -1, -1, NULL);
        first = 0;
    }

    /* 5) Disassembler tier (Capstone) — annotation capability. */
    {
        int avail = asmtest_disas_available();
        row(first, "disasm", "capstone", "host", "host", avail,
            avail ? "" : "built without Capstone", "n/a", -1, -1, -1, NULL);
        first = 0;
    }

    printf("\n  ]\n}\n");
    return 0;
}
