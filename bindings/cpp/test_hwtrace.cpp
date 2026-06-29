/*
 * test_hwtrace.cpp — standalone live test for the single-step hardware-trace
 * backend via the C++ wrapper (asmtest_hwtrace.hpp). Mirrors the Python suite
 * bindings/python/tests/test_hwtrace.py.
 *
 * Unlike the DynamoRIO wrapper (which needs a DynamoRIO install) and the
 * PT/AMD/CoreSight backends (which need specific bare-metal hardware), the
 * SINGLESTEP backend runs on ANY x86-64 Linux — so this asserts a real, live trace
 * here and in CI/containers, self-skipping (prints "# SKIP <reason>", returns 0)
 * only off x86-64 Linux, without Capstone, or when the library is absent.
 *
 * Emits TAP-style "ok N - ..." / "not ok N - ..." lines and exits nonzero on any
 * failure. Build standalone (links only -ldl):
 *
 *   g++ -std=c++17 -I include bindings/cpp/test_hwtrace.cpp -ldl -o test_hwtrace
 *   ASMTEST_HWTRACE_LIB=$PWD/build/libasmtest_hwtrace.so ./test_hwtrace
 *
 * IMPORTANT: under SINGLESTEP, EFLAGS.TF is armed across begin..call..end — the
 * region scopes below keep that window tight (no I/O between begin and end).
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "asmtest_hwtrace.hpp"

using namespace asmtest;

// mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret  (two basic blocks)
static const std::vector<std::uint8_t> ROUTINE = {
    0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0x48, 0x3D, 0x64, 0x00,
    0x00, 0x00, 0x7E, 0x03, 0x48, 0xFF, 0xC8, 0xC3};

// mov rax,0; L: add rax,rdi; dec rsi; jnz L; ret  (19 back-edges > LBR's 16)
static const std::vector<std::uint8_t> LOOP = {
    0x48, 0xC7, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x48,
    0x01, 0xF8, 0x48, 0xFF, 0xCE, 0x75, 0xF8, 0xC3};

static int g_test = 0;
static int g_failed = 0;

static void ok(bool cond, const char *msg) {
    ++g_test;
    if (cond) {
        std::printf("ok %d - %s\n", g_test, msg);
    } else {
        std::printf("not ok %d - %s\n", g_test, msg);
        ++g_failed;
    }
}

int main() {
    if (!HwTrace::available(SINGLESTEP)) {
        std::printf("# SKIP single-step backend unavailable: %s\n",
                    HwTrace::skip_reason(SINGLESTEP).c_str());
        return 0;
    }

    HwTrace::init(SINGLESTEP);

    // ---- ROUTINE: two-block branch fixture ----
    {
        NativeCode code = NativeCode::from_bytes(ROUTINE);
        HwTrace tr = HwTrace::create(/*blocks=*/64, /*instructions=*/64);
        tr.register_region("add2", code);

        long result;
        {
            auto scope = tr.region("add2");
            result = code.call(20, 22);  // 42 <= 100 -> jle taken, dec skipped
        }

        ok(result == 42, "ROUTINE: code.call(20, 22) == 42");
        // Byte-for-byte the Unicorn/DynamoRIO/PT/AMD result for this fixture.
        const std::vector<std::uint64_t> expect{0x0, 0x3, 0x6, 0xC, 0x11};
        ok(tr.insn_offsets() == expect,
           "ROUTINE: insn_offsets() == {0, 3, 6, 0xC, 0x11}");
        ok(tr.insns_total() == 5, "ROUTINE: insns_total() == 5");
        ok(tr.covered(0) && tr.covered(0x11),
           "ROUTINE: covered(0) && covered(0x11)");
        ok(tr.blocks_len() == 2, "ROUTINE: blocks_len() == 2");
        ok(!tr.truncated(), "ROUTINE: !truncated()");

        tr.free();
        code.free();
    }

    // ---- LOOP: tight loop past the LBR depth ceiling ----
    {
        NativeCode code = NativeCode::from_bytes(LOOP);
        HwTrace tr = HwTrace::create(/*blocks=*/64, /*instructions=*/256);
        tr.register_region("loop", code);

        long result;
        {
            auto scope = tr.region("loop");
            result = code.call(1, 20);
        }

        ok(result == 20, "LOOP: code.call(1, 20) == 20");
        ok(tr.insns_total() == 62,
           "LOOP: insns_total() == 62 (1 + 20*3 + 1)");
        ok(tr.covered(0) && tr.covered(0x7),
           "LOOP: covered(0) && covered(0x7)");
        ok(tr.blocks_len() == 2, "LOOP: blocks_len() == 2");
        ok(!tr.truncated(), "LOOP: !truncated()");

        tr.free();
        code.free();
    }

    HwTrace::shutdown();

    // ---- auto-select front-end: pick the most-faithful available backend ----
    // Mirrors examples/test_hwtrace.c's test_auto_resolve: selection invariants
    // plus a live traced call through whatever auto picks (single-step here).
    {
        std::vector<int> best = HwTrace::resolve(BEST);
        std::vector<int> cf = HwTrace::resolve(CEILING_FREE);

        bool ok_avail = true, ok_order = true;
        for (std::size_t i = 0; i < best.size(); ++i) {
            if (!HwTrace::available(best[i]))
                ok_avail = false;
            if (i && best[i] <= best[i - 1])
                ok_order = false;
        }
        ok(ok_avail, "auto: resolve(BEST) returns only available backends");
        ok(ok_order, "auto: resolve(BEST) ordered by descending fidelity, no dups");

        bool cf_no_amd = true, cf_subset = true;
        for (int b : cf) {
            if (b == AMD_LBR)
                cf_no_amd = false;
            bool in_best = false;
            for (int x : best)
                in_best = in_best || (x == b);
            cf_subset = cf_subset && in_best;
        }
        ok(cf_no_amd, "auto: CEILING_FREE never selects AMD LBR");
        ok(cf_subset, "auto: CEILING_FREE is a subset of BEST");

        int ab = HwTrace::auto_select(BEST);
        bool head_ok = best.empty() ? (ab == ASMTEST_HW_EUNAVAIL) : (ab == best[0]);
        ok(head_ok, "auto: auto_select(BEST) is the head of resolve(BEST)");

        // single-step keeps the cascade non-empty on this x86-64 Linux host.
        ok(!best.empty() && ab >= 0,
           "auto: resolves a backend (single-step floor)");

        if (ab >= 0) {
            HwTrace::init(ab);
            NativeCode code = NativeCode::from_bytes(ROUTINE);
            HwTrace tr = HwTrace::create(/*blocks=*/64, /*instructions=*/64);
            tr.register_region("auto", code);
            long result;
            {
                auto scope = tr.region("auto");
                result = code.call(20, 22);
            }
            ok(result == 42, "auto: auto-selected backend traces a live call (== 42)");
            ok(tr.covered(0), "auto: auto-selected backend covers block offset 0");
            if (ab == SINGLESTEP) {
                const std::vector<std::uint64_t> expect{0x0, 0x3, 0x6, 0xC, 0x11};
                ok(tr.insn_offsets() == expect,
                   "auto: single-step pick yields [0, 3, 6, 0xC, 0x11]");
            }
            tr.free();
            code.free();
            HwTrace::shutdown();
        }
    }

    // ---- cross-tier orchestrator: resolve over hwtrace + DynamoRIO + emulator ----
    // Mirrors test_hwtrace.py's test_cross_tier_resolve_invariants: structural
    // invariants of the full descending-fidelity cascade.
    {
        std::vector<TierChoice> best = HwTrace::resolveTiers(TRACE_BEST);
        std::vector<TierChoice> nat = HwTrace::resolveTiers(TRACE_NATIVE_ONLY);
        std::vector<TierChoice> cf = HwTrace::resolveTiers(TRACE_CEILING_FREE);

        // Every HW choice satisfies the hardware-tier probe; each choice's fidelity
        // is VIRTUAL iff it is the emulator tier, NATIVE otherwise.
        bool hw_avail = true, fidelity_ok = true;
        int emu_count = 0;
        for (const TierChoice &c : best) {
            if (c.tier == TIER_HWTRACE && !HwTrace::available(c.backend))
                hw_avail = false;
            int want = (c.tier == TIER_EMULATOR) ? FIDELITY_VIRTUAL
                                                 : FIDELITY_NATIVE;
            if (c.fidelity != want)
                fidelity_ok = false;
            if (c.tier == TIER_EMULATOR)
                ++emu_count;
        }
        ok(hw_avail, "cross-tier: every HW choice in resolveTiers(BEST) is available");
        ok(fidelity_ok,
           "cross-tier: fidelity is VIRTUAL iff emulator tier, else NATIVE");

        // The emulator floor is the single last entry under BEST.
        ok(!best.empty() && best.back().tier == TIER_EMULATOR,
           "cross-tier: emulator is the last entry under BEST");
        ok(emu_count == 1, "cross-tier: exactly one emulator floor under BEST");

        // NATIVE_ONLY forbids the native->emulator crossing: it is BEST minus floor.
        bool nat_no_emu = true;
        for (const TierChoice &c : nat)
            if (c.tier == TIER_EMULATOR)
                nat_no_emu = false;
        ok(nat_no_emu, "cross-tier: NATIVE_ONLY never selects the emulator tier");
        ok(nat.size() == best.size() - 1,
           "cross-tier: NATIVE_ONLY is BEST minus the emulator floor");

        // CEILING_FREE drops AMD LBR.
        bool cf_no_amd = true;
        for (const TierChoice &c : cf)
            if (c.tier == TIER_HWTRACE && c.backend == AMD_LBR)
                cf_no_amd = false;
        ok(cf_no_amd, "cross-tier: CEILING_FREE never selects AMD LBR");

        // autoTier(policy) is the head of resolveTiers(policy).
        std::optional<TierChoice> one = HwTrace::autoTier(TRACE_BEST);
        bool head_ok = one.has_value() && !best.empty() &&
                       one->tier == best[0].tier &&
                       one->backend == best[0].backend;
        ok(head_ok, "cross-tier: autoTier(BEST) is the head of resolveTiers(BEST)");
    }

    // ---- cross-tier NATIVE_ONLY on x86-64 Linux: single-step is a native floor ----
    // Mirrors test_cross_tier_native_only_resolves_on_linux_x86_64. We are past the
    // single-step availability guard at the top of main(), so the floor is present.
    {
        std::vector<TierChoice> nat = HwTrace::resolveTiers(TRACE_NATIVE_ONLY);
        std::optional<TierChoice> pick = HwTrace::autoTier(TRACE_NATIVE_ONLY);

        ok(!nat.empty() && pick.has_value() &&
               pick->fidelity == FIDELITY_NATIVE,
           "cross-tier: NATIVE_ONLY resolves a native pick on x86-64 Linux");

        bool has_singlestep = false;
        for (const TierChoice &c : nat)
            if (c.tier == TIER_HWTRACE && c.backend == SINGLESTEP)
                has_singlestep = true;
        ok(has_singlestep,
           "cross-tier: NATIVE_ONLY cascade includes the single-step floor");
    }

    std::printf("1..%d\n", g_test);
    return g_failed == 0 ? 0 : 1;
}
