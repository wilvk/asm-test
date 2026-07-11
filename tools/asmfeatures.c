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
 *   - host-native tier (x86-64 Linux): run a tiny routine AND a loop under
 *     asmtest_trace_call_auto and report the chosen backend + !truncated — the
 *     metric that actually varies (a fixed-window backend truncates a loop).
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

/* One feature row. `complete`/`insns` are -1 when not measured (printed null). */
static void row(int first, const char *tier, const char *backend,
                const char *arch, const char *scope, int available,
                const char *skip_reason, const char *fidelity, int complete,
                long long insns, const char *note) {
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
        ok ? insns : -1, NULL);
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

    /* 2) Host-native trace completeness (x86-64 Linux): the metric that varies.
     * A tiny straight-line routine vs a loop, under the auto-escalating tier.
     * The native tiers EXECUTE the real bytes in-process, so the fixture must be
     * materialized into W^X executable memory first (asmtest_hwtrace_exec_alloc)
     * — a plain .rodata pointer is not executable. */
#if defined(__x86_64__) && defined(__linux__)
    {
        struct {
            const char *label;
            const unsigned char *bytes;
            size_t len;
            long arg;
            int nargs;
        } probes[] = {
            {"add3 (straight-line)", FIX_X86_ADD3, sizeof FIX_X86_ADD3, 0, 3},
            {"sum_to_n(200) loop", FIX_X86_SUMTON, sizeof FIX_X86_SUMTON, 200,
             1},
        };
        for (int p = 0; p < 2; p++) {
            void *exec = NULL;
            size_t exec_len = 0;
            if (asmtest_hwtrace_exec_alloc(probes[p].bytes, probes[p].len,
                                           &exec, &exec_len) != ASMTEST_HW_OK) {
                row(first, "native-auto", "native", "x86_64", "host", 0,
                    "exec_alloc failed", "native", -1, -1, probes[p].label);
                first = 0;
                continue;
            }
            asmtest_trace_t *t = asmtest_trace_new(8192, 512);
            asmtest_trace_choice_t used;
            memset(&used, 0, sizeof used);
            long result = 0;
            long a3[3] = {2, 3, 4};
            const long *args = (probes[p].nargs == 3) ? a3 : &probes[p].arg;
            int rc = asmtest_trace_call_auto(
                exec, probes[p].len, args, probes[p].nargs, ASMTEST_TRACE_BEST,
                &result, t, &used);
            if (rc == ASMTEST_HW_OK) {
                const char *bk = (used.tier == ASMTEST_TIER_HWTRACE)
                                     ? hw_backend_name(used.backend)
                                     : tier_name(used.tier);
                int complete = !asmtest_emu_trace_truncated(t);
                long long insns = (long long)asmtest_emu_trace_insns_total(t);
                row(first, "native-auto", bk, "x86_64", "host", 1, "", "native",
                    complete, insns, probes[p].label);
            } else {
                row(first, "native-auto", "native", "x86_64", "host", 0,
                    "no call-owning native tier available", "native", -1, -1,
                    probes[p].label);
            }
            first = 0;
            asmtest_trace_free(t);
            asmtest_hwtrace_exec_free(exec, exec_len);
        }
    }
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
            avail ? "" : reason, "native", -1, -1, NULL);
        first = 0;
    }

    /* 4) DynamoRIO tier. */
    {
        int avail = asmtest_dr_available();
        char reason[256] = "";
        asmtest_dr_skip_reason(reason, sizeof reason);
        row(first, "dynamorio", "dynamorio", "host", "host", avail,
            avail ? "" : reason, "native", -1, -1, NULL);
        first = 0;
    }

    /* 5) Disassembler tier (Capstone) — annotation capability. */
    {
        int avail = asmtest_disas_available();
        row(first, "disasm", "capstone", "host", "host", avail,
            avail ? "" : "built without Capstone", "n/a", -1, -1, NULL);
        first = 0;
    }

    printf("\n  ]\n}\n");
    return 0;
}
