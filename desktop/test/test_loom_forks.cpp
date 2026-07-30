// test_loom_forks.cpp — the fork engine and the end-to-end fork differential
// (05-loom-day-one.md T5, and T7's steps 4-5). FULL BUILD ONLY: this binary
// links Unicorn (emu_*, the L0 producer) and Keystone (asmtest_assemble), which
// is exactly the licensing boundary D4 draws — nothing in `asmtest-viewer`
// links any of it.
//
// The fixture is the doc's `fork_demo`, assembled here rather than hand-encoded
// so the test cannot drift from the listing it documents:
//
//   0x00  mov rdx, rdi   ; hot: the value differs across takes
//   0x03  shr rdx, 63    ; DIMMED: 0 for both non-negative arguments
//   0x07  mov rax, rdi   ; hot
//   0x0a  cmp rax, 10    ; hot (the flags differ)
//   0x0e  jle 0x13       ; last aligned step
//   0x10  neg rax        ; runs only when a > 10
//   0x13  ret
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "asmtest_assemble.h"
#include "loom/forks.h"
#include "loom/take_view.h"

using namespace asmdesk;

static int failures;
static void fail(const std::string &what, const std::string &why) {
    std::fprintf(stderr, "FAIL %s: %s\n", what.c_str(), why.c_str());
    failures++;
}
static void check(const std::string &what, bool cond, const std::string &why) {
    if (!cond)
        fail(what, why);
}

// examples/test_dataflow_emu.c:38 — the chain the whole doc walks through.
static const uint8_t df_chain[] = {
    0x48, 0x89, 0xf8,             /* 0x00 mov rax, rdi        */
    0x48, 0x89, 0x44, 0x24, 0xf8, /* 0x03 mov [rsp-8], rax    */
    0x48, 0x8b, 0x4c, 0x24, 0xf8, /* 0x08 mov rcx, [rsp-8]    */
    0x48, 0x8d, 0x14, 0x31,       /* 0x0d lea rdx, [rcx+rsi]  */
    0x48, 0x89, 0xd0,             /* 0x11 mov rax, rdx        */
    0xc3,                         /* 0x14 ret                 */
};

static const char *kForkDemo = "mov rdx, rdi\n"
                               "shr rdx, 63\n"
                               "mov rax, rdi\n"
                               "cmp rax, 10\n"
                               "jle 0x100013\n"
                               "neg rax\n"
                               "ret\n";
static const char *kForkDemoPatched = "mov rdx, rdi\n"
                                      "shr rdx, 1\n"
                                      "mov rax, rdi\n"
                                      "cmp rax, 10\n"
                                      "jle 0x100013\n"
                                      "neg rax\n"
                                      "ret\n";

static std::vector<uint8_t> assemble(const char *src, const char *what) {
    asm_result_t r{};
    asmtest_assemble(ASM_X86_64, ASM_SYNTAX_INTEL, src, EMU_CODE_BASE, &r);
    std::vector<uint8_t> out;
    if (!r.ok)
        fail(std::string("assemble ") + what, r.err);
    else
        out.assign(r.bytes, r.bytes + r.len);
    asmtest_asm_free(&r);
    return out;
}

static loom_fabric_t weave(const loom_take_t &t) {
    loom_fabric_t f;
    std::string err;
    if (!loom_fabric_build(t.vt(), t.g(), t.provenance(), &f, &err))
        fail("weave take", err);
    return f;
}

int main() {
    emu_t *e = emu_open();
    if (e == nullptr) {
        std::fprintf(stderr, "test_loom_forks: FAIL: emu_open returned NULL\n");
        return 1;
    }
    emu_snapshot_t *base_state = emu_snapshot(e);
    if (base_state == nullptr) {
        std::fprintf(stderr, "test_loom_forks: FAIL: emu_snapshot failed\n");
        return 1;
    }

    const long df_args[2] = {7, 5};

    // --- T5: an entry-arg take re-runs from entry --------------------------
    {
        loom_edit_t edit;
        edit.k = loom_edit_t::kind::entry_arg;
        edit.arg_index = 0;
        edit.arg_value = 9;
        loom_take_t take;
        check("arg take runs",
              loom_take_run(e, base_state, df_chain, sizeof df_chain, df_args,
                            2, edit, &take),
              take.err);
        check("no loud error on a clean take", take.err.empty(), take.err);
        check("the take is a 6-step fabric", take.insn_off.size() == 6,
              "got " + std::to_string(take.insn_off.size()));

        loom_fabric_t f = weave(take);
        // The store at step 1 now carries 9, not 7 — one fact changed, and the
        // whole worldline follows.
        bool chip9 = false;
        for (const loom_span_t &s : f.spans)
            if (f.lanes[s.lane].kind == loom_lane_kind::mem_band &&
                s.t_write == 1)
                chip9 = s.value_valid && s.value == 9;
        check("the step-1 store carries chip 9", chip9, loom_fabric_dump(f));
        check("the take is badged as an isolated guest",
              f.prov.isolated_guest && f.prov.exact, "");

        // --- determinism: the same take twice is byte-identical -------------
        loom_take_t again;
        loom_take_run(e, base_state, df_chain, sizeof df_chain, df_args, 2,
                      edit, &again);
        check("the same take twice yields identical step offsets",
              again.insn_off == take.insn_off, "");
        check("...identical operand records",
              again.recs.size() == take.recs.size() &&
                  std::memcmp(again.recs.data(), take.recs.data(),
                              take.recs.size() * sizeof(at_val_rec_t)) == 0,
              "");
        check("...and identical fabrics",
              loom_fabric_dump(weave(again)) == loom_fabric_dump(f), "");
    }

    // --- T5: an out-of-range arg index is refused loudly -------------------
    {
        loom_edit_t edit;
        edit.arg_index = 4;
        loom_take_t take;
        check("an argument the routine does not take is refused",
              !loom_take_run(e, base_state, df_chain, sizeof df_chain, df_args,
                             2, edit, &take),
              "it ran anyway");
        check("the refusal names the argument",
              take.err.find("arg4") != std::string::npos, take.err);
    }

    // --- T5: the loud-drop contract turns a bad listing into a fork failure -
    {
        loom_edit_t edit;
        edit.k = loom_edit_t::kind::code_patch;
        edit.syntax = ASM_SYNTAX_INTEL;
        // AT&T text under the Intel default — the documented common case
        // (src/assemble.c:239). Keystone reports SUCCESS after silently
        // dropping the AT&T line; asmtest_assemble's statement count is the
        // only witness, and it refuses the whole assemble.
        //
        // Measured on the pinned Keystone (2026-07-24): a truncated operand
        // such as `mov rax, [` is NOT this case — Keystone accepts it and emits
        // 3 statements / 14 bytes, so nothing was dropped and there is nothing
        // to be loud about. The loud-drop guarantee is about statements the
        // assembler SKIPS, not about text it accepts and reads differently than
        // the author meant; a fixture that conflated the two would be testing
        // a promise nothing makes.
        edit.patched_source = "mov rax, rdi\nmovq %rsi, %rax\nret\n";
        loom_take_t take;
        check("an unassemblable patch fails the take",
              !loom_take_run(e, base_state, df_chain, sizeof df_chain, df_args,
                             2, edit, &take),
              "a fabric of code the user did not write was produced");
        check("the failure is loud and non-empty", !take.err.empty(), "");
        check("the failure names the dropped statements",
              take.err.find("skipped") != std::string::npos, take.err);
        check("no fabric input survives a failed take",
              take.insn_off.empty() && take.recs.empty(), "");
    }

    // --- T5: a faulting patch fills the fault card -------------------------
    {
        loom_edit_t edit;
        edit.k = loom_edit_t::kind::code_patch;
        edit.syntax = ASM_SYNTAX_INTEL;
        edit.patched_source = "xor rax, rax\nmov rbx, [rax]\nret\n";
        loom_take_t take;
        check("a faulting patch still produces a take",
              loom_take_run(e, base_state, df_chain, sizeof df_chain, df_args,
                            2, edit, &take),
              take.err);
        check("the take faulted", take.result.faulted,
              "the emulator did not report a fault");
        check("the fault is a READ at 0",
              take.result.fault_kind == EMU_FAULT_READ &&
                  take.result.fault_addr == 0,
              "kind=" + std::to_string(take.result.fault_kind) +
                  " addr=" + std::to_string(take.result.fault_addr));
        std::string card = take.fault_card();
        check("the fault card is data, not a crash", !card.empty(), "");
        check("the fault card names the direction",
              card.find("read fault") != std::string::npos, card);
    }

    // --- T7 step 4: the fork differential, end to end ----------------------
    std::vector<uint8_t> demo = assemble(kForkDemo, "fork_demo");
    check("fork_demo assembles to the documented 0x14 bytes",
          demo.size() == 0x14, "got " + std::to_string(demo.size()));
    // The golden generator carries the SAME routine as a byte literal
    // (tools/asmtrace_record.c LOOM_FORK_DEMO), because the recorder links no
    // assembler. This is the cross-check that keeps the two from drifting —
    // the doc's "hand-verify the encodings once against a disassembler" step,
    // as a test rather than a promise.
    {
        static const uint8_t committed[] = {
            0x48, 0x89, 0xfa, 0x48, 0xc1, 0xea, 0x3f, 0x48, 0x89, 0xf8,
            0x48, 0x83, 0xf8, 0x0a, 0x7e, 0x03, 0x48, 0xf7, 0xd8, 0xc3,
        };
        check("the assembled fork_demo equals the generator's byte literal",
              demo.size() == sizeof committed &&
                  std::memcmp(demo.data(), committed, sizeof committed) == 0,
              "tools/asmtrace_record.c's LOOM_FORK_DEMO has drifted from the "
              "listing this test assembles");
    }
    if (!demo.empty()) {
        const long a3[1] = {3};
        loom_edit_t none;
        none.arg_index = 0;
        none.arg_value = 3; // the identity edit: the base run
        loom_take_t base_take, take11;
        loom_take_run(e, base_state, demo.data(), demo.size(), a3, 1, none,
                      &base_take);
        loom_edit_t to11;
        to11.arg_index = 0;
        to11.arg_value = 11;
        loom_take_run(e, base_state, demo.data(), demo.size(), a3, 1, to11,
                      &take11);

        check("the base run took the jle arm (6 steps)",
              base_take.insn_off.size() == 6,
              "got " + std::to_string(base_take.insn_off.size()));
        check("the take fell through into neg (7 steps)",
              take11.insn_off.size() == 7,
              "got " + std::to_string(take11.insn_off.size()));

        loom_fabric_t fb = weave(base_take), ft = weave(take11);
        loom_edit_desc_t desc;
        desc.arg_reg = 39; // rdi
        desc.label = to11.describe();
        loom_take_view_t v = loom_take_view(fb, ft, take11.edges.data(),
                                            take11.edges.size(), desc, "", "");
        std::string dump = loom_take_dump(v);

        check("the aligned prefix is 5", v.prefix == 5,
              "got " + std::to_string(v.prefix) + "\n" + dump);
        check("patient zero is at index 5", v.div.diverged && v.div.step == 5,
              dump);
        check("patient zero is 0x13 (base) vs 0x10 (take)",
              v.div.off_a == 0x13 && v.div.off_b == 0x10, dump);

        auto at = [&](uint32_t s) -> const loom_take_step_t * {
            for (const loom_take_step_t &x : v.steps)
                if (x.step == s)
                    return &x;
            return nullptr;
        };
        // If the pinned Unicorn ever makes the takes' post-`shr` values differ,
        // this must FAIL loudly — swap the fixture instruction and regenerate.
        // Never loosen it: the dim verdict is the doc's whole demonstration
        // that dependence and consequence are different things.
        check("step 1 (shr rdx, 63) is DIM",
              at(1) != nullptr && at(1)->verdict == take_verdict::dim, dump);
        check("step 0 (mov rdx, rdi) is HOT",
              at(0) != nullptr && at(0)->verdict == take_verdict::hot, dump);
        check("take steps from index 5 are unaligned",
              at(5) != nullptr && at(5)->unaligned, dump);

        // The whole run twice -> byte-identical plans.
        loom_take_t again;
        loom_take_run(e, base_state, demo.data(), demo.size(), a3, 1, to11,
                      &again);
        loom_fabric_t ft2 = weave(again);
        loom_take_view_t v2 = loom_take_view(fb, ft2, again.edges.data(),
                                             again.edges.size(), desc, "", "");
        loom_view_t cam;
        cam.steps_per_px = 0.05;
        cam.px_w = 800;
        cam.px_h = 200;
        std::vector<loom_prim_t> p1, p2;
        loom_plan(ft, cam, &p1);
        loom_take_plan(v, cam, &p1);
        loom_plan(ft2, cam, &p2);
        loom_take_plan(v2, cam, &p2);
        check("the whole run twice yields byte-identical plans",
              loom_plan_dump(p1) == loom_plan_dump(p2), "");

        // --- T7 step 5: the code-patch leg ---------------------------------
        loom_edit_t patch;
        patch.k = loom_edit_t::kind::code_patch;
        patch.syntax = ASM_SYNTAX_INTEL;
        patch.patched_source = kForkDemoPatched;
        loom_take_t patched;
        check("the patched listing assembles and runs",
              loom_take_run(e, base_state, demo.data(), demo.size(), a3, 1,
                            patch, &patched),
              patched.err);
        loom_fabric_t fp = weave(patched);
        loom_edit_desc_t pdesc;
        pdesc.code_patch = true;
        pdesc.label = "shr rdx, 63 -> shr rdx, 1";
        pdesc.base_code = demo;
        pdesc.take_code = patched.code;
        loom_take_view_t vp = loom_take_view(
            fb, fp, patched.edges.data(), patched.edges.size(), pdesc, "", "");
        std::string pdump = loom_take_dump(vp);
        check("the code-patch cone seeds at step 1",
              !vp.cone.empty() && vp.cone.front() == 1, pdump);
        for (const loom_take_step_t &x : vp.steps)
            if (x.step == 1)
                check("step 1 goes HOT under the patch",
                      x.verdict == take_verdict::hot, pdump);

        // ...and a deliberately broken listing surfaces the loud-drop error in
        // the fork CARD, verbatim — not as an empty fabric the user must
        // interpret.
        loom_edit_t broken;
        broken.k = loom_edit_t::kind::code_patch;
        broken.syntax = ASM_SYNTAX_INTEL;
        broken.patched_source = "mov rdx, rdi\nmovq %rdi, %rax\nret\n";
        loom_take_t bad;
        check("the broken listing fails the take",
              !loom_take_run(e, base_state, demo.data(), demo.size(), a3, 1,
                             broken, &bad),
              "it ran");
        loom_take_view_t vb =
            loom_take_view(fb, loom_fabric_t{}, nullptr, 0, pdesc, "", bad.err);
        check("the fork card carries the assembler's own words",
              vb.node.err == bad.err && !vb.node.err.empty(), vb.node.err);
        check("...which name the drop",
              vb.node.err.find("skipped") != std::string::npos, vb.node.err);
    }

    // --- 30 R3 T3: Reweave — fork from a chosen execution step K -----------
    // A one-register edit at step K, resumed forward. Straight-line code keeps
    // the offsets aligned and diverges in VALUES; the fidelity label is mandatory.
    {
        // poly(a,b,c) — straight-line; step 3 (imul) is the reweave point.
        //   0 mov rax,rdi  1 add rax,rsi  2 add rax,rdx  3 imul rax,rax
        //   4 mov [rsp-8],rax  5 mov rcx,[rsp-8]  6 add rax,rcx  7 ret
        static const uint8_t poly[] = {
            0x48, 0x89, 0xf8,             0x48, 0x01, 0xf0, 0x48, 0x01, 0xd0,
            0x48, 0x0f, 0xaf, 0xc0,       0x48, 0x89, 0x44, 0x24, 0xf8,
            0x48, 0x8b, 0x4c, 0x24, 0xf8, 0x48, 0x01, 0xc8, 0xc3,
        };
        const long pa[3] = {2, 3, 4};

        // The parent (entry) run the take-view diverges against.
        loom_edit_t ident;
        ident.arg_index = 0;
        ident.arg_value = 2; // the identity edit
        loom_take_t parent;
        loom_take_run(e, base_state, poly, sizeof poly, pa, 3, ident, &parent);
        check("poly parent is an 8-step run", parent.insn_off.size() == 8,
              "got " + std::to_string(parent.insn_off.size()));

        // Reweave: rax := 0 at step 3 (pre-imul; rax held a+b+c = 9).
        loom_from_step_edit_t edit;
        edit.k = loom_from_step_edit_t::kind::reg;
        edit.step = 3;
        edit.reg = "rax";
        edit.reg_value = 0;
        loom_take_t rw;
        check("the reweave runs",
              loom_take_run_from_step(poly, sizeof poly, pa, 3, edit, &rw),
              rw.err);
        check("no loud error on a clean reweave", rw.err.empty(), rw.err);
        check("the reweave is a full 8-step worldline (straight-line)",
              rw.insn_off.size() == 8,
              "got " + std::to_string(rw.insn_off.size()));
        check("the reweave shares the parent's instruction spine (values-only "
              "divergence)",
              rw.insn_off == parent.insn_off, "offsets moved");
        check("the reweave is badged as a resumed isolated-guest replay",
              rw.provenance().producer.find("reweave@3") != std::string::npos &&
                  rw.provenance().isolated_guest && rw.provenance().exact,
              rw.provenance().producer);

        loom_fabric_t fp = weave(parent), fr = weave(rw);
        loom_edit_desc_t d;
        d.from_step_edit = true;
        d.from_step = 3;
        d.label = edit.describe();
        loom_take_view_t v =
            loom_take_view(fp, fr, rw.edges.data(), rw.edges.size(), d, "", "");
        std::string dump = loom_take_dump(v);
        check("straight-line reweave is aligned end-to-end (no offset patient "
              "zero)",
              !v.div.diverged &&
                  v.node.alignment == std::string(kLoomAlignedEndToEnd),
              dump);
        // The MANDATORY fidelity label (D7): a reweave IS emulator replay, never
        // observed silicon — it carries the same crossing-the-line disclosure.
        check("the reweave carries the crossing-the-line disclosure",
              v.node.disclosure == std::string(kLoomForkDisclosure),
              v.node.disclosure);
        check("the cone seeds at the reweave step (K=3)",
              !v.cone.empty() && v.cone.front() == 3, dump);
        bool any_hot = false;
        for (const loom_take_step_t &x : v.steps)
            if (x.in_cone && x.verdict == take_verdict::hot)
                any_hot = true;
        check("the reweave's cone carries a real value divergence (HOT)",
              any_hot, dump);

        // Determinism: the same reweave twice is byte-identical (hermetic).
        loom_take_t rw2;
        loom_take_run_from_step(poly, sizeof poly, pa, 3, edit, &rw2);
        check("the same reweave twice yields identical offsets",
              rw2.insn_off == rw.insn_off, "");
        check("...and identical operand records",
              rw2.recs.size() == rw.recs.size() &&
                  std::memcmp(rw2.recs.data(), rw.recs.data(),
                              rw.recs.size() * sizeof(at_val_rec_t)) == 0,
              "");

        // A step past the run's end is refused loudly (no checkpoint there).
        loom_from_step_edit_t far;
        far.step = 999;
        far.reg = "rax";
        loom_take_t oob;
        check("a step past the run's end is refused",
              !loom_take_run_from_step(poly, sizeof poly, pa, 3, far, &oob),
              "it ran anyway");
        check("the refusal names the missing checkpoint",
              oob.err.find("no checkpoint") != std::string::npos, oob.err);

        // An unknown register name is refused loudly.
        loom_from_step_edit_t badreg;
        badreg.step = 3;
        badreg.reg = "rzx";
        loom_take_t br;
        check("an unknown register is refused",
              !loom_take_run_from_step(poly, sizeof poly, pa, 3, badreg, &br),
              "it ran anyway");
        check("the refusal names the register",
              br.err.find("rzx") != std::string::npos, br.err);

        // A memory reweave: poke the spilled slot [rsp-8] AFTER the store (step 5
        // reloads it into rcx), so the counterfactual reaches through memory.
        loom_from_step_edit_t medit;
        medit.k = loom_from_step_edit_t::kind::mem;
        medit.step = 5; // pre-`mov rcx,[rsp-8]`
        medit.mem_addr = 0x20fff8 - 8; // DF_STACK top - 8 = [rsp-8]
        medit.mem_bytes.assign(8, 0);  // zero the slot
        loom_take_t mrw;
        check("a memory reweave runs",
              loom_take_run_from_step(poly, sizeof poly, pa, 3, medit, &mrw),
              mrw.err);
        check("the memory reweave is a full 8-step worldline",
              mrw.insn_off.size() == 8,
              "got " + std::to_string(mrw.insn_off.size()));
    }

    // --- 30 R3 T3: a control-flow reweave -> patient zero at/after K -------
    if (!demo.empty()) {
        const long a3[1] = {3};
        loom_edit_t ident;
        ident.arg_index = 0;
        ident.arg_value = 3;
        loom_take_t parent;
        loom_take_run(e, base_state, demo.data(), demo.size(), a3, 1, ident,
                      &parent);
        // rax := 11 at step 3 (pre-`cmp rax,10`) flips the jle: the base takes
        // the branch (6 steps), the reweave falls through into neg (7 steps).
        loom_from_step_edit_t edit;
        edit.step = 3;
        edit.reg = "rax";
        edit.reg_value = 11;
        loom_take_t rw;
        check("the control-flow reweave runs",
              loom_take_run_from_step(demo.data(), demo.size(), a3, 1, edit,
                                      &rw),
              rw.err);
        check("the base took the jle arm (6 steps)", parent.insn_off.size() == 6,
              "got " + std::to_string(parent.insn_off.size()));
        check("the reweave fell through into neg (7 steps)",
              rw.insn_off.size() == 7,
              "got " + std::to_string(rw.insn_off.size()));
        loom_fabric_t fp = weave(parent), fr = weave(rw);
        loom_edit_desc_t d;
        d.from_step_edit = true;
        d.from_step = 3;
        d.label = edit.describe();
        loom_take_view_t v =
            loom_take_view(fp, fr, rw.edges.data(), rw.edges.size(), d, "", "");
        std::string dump = loom_take_dump(v);
        check("patient zero appears at or after the edit step (K=3)",
              v.div.diverged && v.div.step >= 3, dump);
        check("patient zero is base 0x13 (ret) vs take 0x10 (neg)",
              v.div.off_a == 0x13 && v.div.off_b == 0x10, dump);
        check("the control-flow reweave carries the disclosure too",
              v.node.disclosure == std::string(kLoomForkDisclosure),
              v.node.disclosure);
    }

    emu_snapshot_free(base_state);
    emu_close(e);
    if (failures) {
        std::fprintf(stderr, "%d loom fork check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_loom_forks: all checks passed\n");
    return 0;
}
