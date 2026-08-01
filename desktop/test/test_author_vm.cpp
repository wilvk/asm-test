// test_author_vm.cpp — the Author door's three rules
// (docs/internal/archive/gui/06-doors-and-learning.md T5). Synthetic inputs only: no
// Keystone, no Unicorn, no display — so the rules are pinned on every host.
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

#include "author_vm.h"
#include "doc/recording.h" // author_recording round-trip (T3)

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

int main() {
    // --- RULE 1: the assembler's diagnostic survives VERBATIM -------------
    {
        // The real loud-drop message (src/assemble.c:239) — the one that names
        // the trap a beginner actually hits: AT&T text under the Intel default.
        const char *real =
            "assembler skipped 1 of 3 statements (check the syntax argument)";
        asm_result_t r;
        std::memset(&r, 0, sizeof r);
        r.ok = false;
        std::snprintf(r.err, sizeof r.err, "%s", real);

        author_result_t a = author_from_assemble(&r);
        check("a failed assemble is not 'assembled'", !a.assembled, "");
        check("the diagnostic survives byte for byte", a.asm_error == real,
              a.asm_error);
        check("the door adds no wrapper text",
              a.asm_error.find("assembly failed") == std::string::npos,
              a.asm_error);
        check("no bytes are shown for a failed assemble",
              a.bytes == 0 && a.hexdump.empty(), a.hexdump);
        check("the dump names the failure",
              author_dump(a).find("assemble FAILED") != std::string::npos,
              author_dump(a));
    }

    // A successful assemble shows its bytes even before it is run.
    uint8_t code[] = {0x48, 0x89, 0xf8, 0xc3};
    asm_result_t ok;
    std::memset(&ok, 0, sizeof ok);
    ok.ok = true;
    ok.bytes = code;
    ok.len = sizeof code;
    {
        author_result_t a = author_from_assemble(&ok);
        check("a clean assemble reports its length", a.bytes == 4, "");
        check("...and hexdumps them", a.hexdump == "48 89 f8 c3", a.hexdump);
        check("...with no error text", a.asm_error.empty(), a.asm_error);
    }

    // --- RULE 2: a fault is a CARD, mapped field by field -----------------
    {
        emu_result_t res;
        std::memset(&res, 0, sizeof res);
        res.faulted = true;
        res.fault_kind = EMU_FAULT_READ;
        res.fault_addr = 0;
        res.regs.rip = 0x100003;
        res.regs.rax = 0;
        res.regs.rbx = 0xdead;

        author_result_t a = author_from_assemble(&ok);
        author_apply_run(a, ASM_X86_64, &res, "mov rbx, qword ptr [rax]",
                         false);
        check("the run happened", a.ran, "");
        check("the fault is recorded", a.faulted, "");
        check("the kind maps to READ", a.fault_kind == "read", a.fault_kind);
        check("the address maps", a.fault_addr == 0, "");
        check("the register file comes through",
              a.rip == 0x100003 && a.rbx == 0xdead, author_dump(a));
        check("the faulting instruction is shown when recorded",
              a.disasm == "mov rbx, qword ptr [rax]", a.disasm);
        check("the card's copy is verbatim",
              a.fault_note == std::string(kAuthorFaultCopy), a.fault_note);
        check("the copy names the emulator's contribution",
              std::string(kAuthorFaultCopy) ==
                  "the emulator turned this into data; on real hardware this "
                  "would have been a crash",
              kAuthorFaultCopy);
        check("the dump renders the card",
              author_dump(a).find("fault read at 0x0") != std::string::npos,
              author_dump(a));

        // D10: without Capstone the card degrades to the register file and the
        // address — it does not invent a mnemonic.
        author_result_t b = author_from_assemble(&ok);
        author_apply_run(b, ASM_X86_64, &res, "", false);
        check("no recorded disassembly degrades cleanly", b.disasm.empty(), "");
        check("...and the card still names kind and address",
              b.faulted && b.fault_kind == "read", author_dump(b));
    }

    // A clean run reports no fault, and the cap is REPORTED when it fires.
    {
        emu_result_t res;
        std::memset(&res, 0, sizeof res);
        res.ok = true;
        res.regs.rax = 42;
        author_result_t a = author_from_assemble(&ok);
        author_apply_run(a, ASM_X86_64, &res, "", false);
        check("a clean run has no fault card", !a.faulted, author_dump(a));
        check("the result register is shown", a.rax == 42, "");

        author_result_t c = author_from_assemble(&ok);
        author_apply_run(c, ASM_X86_64, &res, "", true);
        check("a capped run says the run did NOT finish",
              c.capped &&
                  author_dump(c).find("did NOT finish") != std::string::npos,
              author_dump(c));
        check("the cap is a real bound", kAuthorMaxInsns > 0, "");
    }

    // --- RULE 3: the arch-gating table ------------------------------------
    {
        check("the table has all four arches", author_arch_table().size() == 4,
              std::to_string(author_arch_table().size()));
        check("x86-64 runs", author_arch(ASM_X86_64)->can_run, "");
        // R5 T3 (32-per-guest-value-producer.md): arm64 now runs too, through
        // the per-guest value-fabric producer — a DIFFERENT result shape from
        // x86-64's emu_result_t (checked below), not just a second "yes".
        check("arm64 runs", author_arch(ASM_ARM64)->can_run, "");
        for (int a : {ASM_RISCV64, ASM_ARM32}) {
            const author_arch_row *row = author_arch(a);
            check(std::string("v1 does not run ") + row->name, !row->can_run,
                  "");
            check(std::string(row->name) + " states the limit on its row",
                  std::string(row->note).find("x86-64/AArch64-only in v1") !=
                      std::string::npos,
                  row->note);
        }
        // A non-runnable arch (ARM32) still assembles, still shows bytes, and
        // carries the labelled limit — never a silent no-op.
        author_result_t a = author_from_assemble(&ok);
        author_apply_run(a, ASM_ARM32, nullptr, "", false);
        check("an ARM32 source still shows its bytes",
              a.hexdump == "48 89 f8 c3", a.hexdump);
        check("...is not marked as run", !a.ran, "");
        check("...and carries the limit note",
              a.limit_note == std::string(kAuthorArchLimit), a.limit_note);
    }

    // --- RULE 3b: the arm64 value fabric is faithful about what it is NOT ----
    // (32-per-guest-value-producer.md R5 T3). author_apply_run_vf is the
    // arm64 counterpart of author_apply_run (RULE 2 above) — same idea
    // (fold a run's outcome into the result), different SHAPE: no register
    // file, no fault kind/address, because the per-guest value-fabric
    // producer captures neither.
    {
        // A clean arm64 run: def-use edges + steps, and NOTHING pretending to
        // be an emu_result_t register/fault snapshot.
        author_valuefabric_t vf;
        vf.ran = true;
        vf.ok = true;
        vf.insn_off = {0, 4, 8, 12};
        vf.edges.resize(2);
        author_result_t a = author_from_assemble(&ok);
        author_apply_run_vf(a, vf);
        check("the value-fabric run is marked as such", a.ran_value_fabric, "");
        check("...but NOT via the emu_result_t 'ran' path (no rax/rbx/… "
              "behind it)",
              !a.ran, "");
        check("...records its step/edge counts",
              a.vf_steps == 4 && a.vf_edges == 2, author_dump(a));
        check("...says what it captured, not what it lacks alone",
              a.vf_note.find("Loom") != std::string::npos, a.vf_note);
        check("no fault card is fabricated for the value-fabric path",
              !a.faulted, "");
        check("the dump renders the value fabric",
              author_dump(a).find("value fabric: 4 step(s), 2 def-use "
                                  "edge(s)") != std::string::npos,
              author_dump(a));

        // The guest did not reach a clean stop: faithful about NOT having a
        // fault kind/address (unlike the x86-64 emu_result_t path).
        author_valuefabric_t bad;
        bad.ran = true;
        bad.ok = false;
        bad.insn_off = {0};
        author_result_t b = author_from_assemble(&ok);
        author_apply_run_vf(b, bad);
        check("an unclean arm64 stop is NOT marked ok", !b.vf_ok, "");
        check("...and names the fault kind/address gap explicitly",
              b.vf_note.find("fault kind or address") != std::string::npos,
              b.vf_note);

        // A truncated fabric says so rather than looking like a clean run.
        author_valuefabric_t trunc;
        trunc.ran = true;
        trunc.ok = true;
        trunc.truncated = true;
        trunc.insn_off = {0};
        author_result_t c = author_from_assemble(&ok);
        author_apply_run_vf(c, trunc);
        check("a truncated arm64 fabric says it is a lower bound",
              c.vf_note.find("lower bound") != std::string::npos, c.vf_note);

        // A producer setup failure degrades gracefully, never silently.
        author_valuefabric_t setup_fail;
        author_result_t d = author_from_assemble(&ok);
        author_apply_run_vf(d, setup_fail);
        check("a setup failure is reported, not silently empty",
              !d.vf_note.empty() && !d.vf_ok, d.vf_note);
    }

    // --- the dialect list -------------------------------------------------
    {
        check("all five dialects are offered",
              author_syntax_table().size() == 5,
              std::to_string(author_syntax_table().size()));
        check("Intel is first, and labelled the default",
              author_syntax_table()[0].syntax == ASM_SYNTAX_INTEL &&
                  std::string(author_syntax_table()[0].name).find("default") !=
                      std::string::npos,
              author_syntax_table()[0].name);
    }

    // --- the render-only tile ---------------------------------------------
    check("the render-only tile names the licence boundary",
          std::string(kAuthorRenderOnly) ==
              "Author mode requires the full (GPL-2.0) build",
          kAuthorRenderOnly);

    // --- 18-T3: the run -> Recording materialisation ----------------------
    // author_recording turns an authored run into a Recording the SAME
    // save_recording_file writes, so Author output stops being ephemeral. Pure
    // JSON assembly, so the round-trip is pinned here without Keystone/Unicorn:
    // the recording carries the authored program image and reloads faithfully.
    {
        author_result_t r;
        r.assembled = true;
        r.bytes = 4;
        // mov rax, rdi ; ret  ->  48 89 f8 c3
        std::vector<uint8_t> image = {0x48, 0x89, 0xf8, 0xc3};
        Recording rec = author_recording(r, ASM_X86_64, image, 0x100000);

        check("mat/exact-provenance",
              rec.provenance.exact && rec.provenance.trust == "exact",
              "an emulator run observed every step — it is exact");
        check("mat/backend-named",
              rec.provenance.backend == "author-emulator", rec.provenance.backend);
        check("mat/arch", rec.arch == "x86_64", rec.arch);
        check("mat/not-torn", rec.has_end && !rec.torn,
              "a finished authored run is a complete recording, not torn");
        check("mat/carries-codeimage", rec.by_kind.count("codeimage") == 1,
              "the assembled program must ride as a codeimage event");

        // The whole point: it round-trips through the real serializer + loader,
        // so a saved authored recording reopens as an equal Recording.
        std::string text = recording_to_asmtrace(rec);
        std::istringstream in(text);
        std::string err;
        auto reloaded = load_recording(in, err);
        check("mat/reloads", reloaded.has_value(),
              std::string("the materialised recording must reload: ") + err);
        if (reloaded) {
            check("mat/reload-not-torn", !reloaded->torn,
                  "a materialised authored recording reloads complete");
            check("mat/reload-keeps-image",
                  reloaded->by_kind.count("codeimage") == 1,
                  "the codeimage survives the save/load round-trip");
            check("mat/reload-arch", reloaded->arch == "x86_64",
                  reloaded->arch);
        }

        // An assemble-only run (no image) still materialises a valid recording.
        Recording empty = author_recording(r, ASM_X86_64, {}, 0x100000);
        std::string etext = recording_to_asmtrace(empty);
        std::istringstream ein(etext);
        std::string eerr;
        check("mat/imageless-reloads", load_recording(ein, eerr).has_value(),
              std::string("an imageless authored recording must still reload: ") +
                  eerr);
    }

    // --- R5 T3: an arm64 value-fabric run -> Recording materialisation -----
    // author_recording's `vf` parameter emits trace/df_step/df_edge events
    // through the SAME writer the codeimage event already uses, so an arm64
    // run opens in the Loom / Slice / Timeline like any other recording — no
    // new serialization invented for it (docs/internal/gui/
    // 32-per-guest-value-producer.md R5 T3).
    {
        author_result_t r2;
        r2.assembled = true;
        r2.bytes = 16;
        std::vector<uint8_t> image2(16, 0);

        // arm64_df_chain(7, 5): add x2,x0,x1 / add x3,x2,x2 / mov x0,x3 / ret
        // (examples/test_dataflow_emu.c's own arm64 fixture) — the exact
        // shape asmtest_dataflow_emu_run_arch would have produced.
        author_valuefabric_t vf;
        vf.ran = true;
        vf.ok = true;
        vf.insn_off = {0, 4, 8, 12};
        vf.disasm = {"add x2, x0, x1", "add x3, x2, x2", "mov x0, x3", "ret"};
        at_val_rec_t w{};
        w.kind = AT_LOC_REG;
        w.reg = 2; // an arbitrary Capstone-shaped reg id for the test
        w.size = 8;
        w.is_write = true;
        w.value_valid = true;
        w.value = 24;
        w.step = 1;
        vf.recs = {w};
        asmtest_defuse_edge_t e{};
        e.from_step = 0;
        e.to_step = 1;
        e.loc = w;
        vf.edges = {e};

        Recording rec = author_recording(r2, ASM_ARM64, image2, 0x100000, &vf);
        check("mat/vf-arch", rec.arch == "aarch64", rec.arch);
        check("mat/vf-carries-trace",
              rec.by_kind.count("trace") == 1 &&
                  rec.by_kind["trace"].size() == 4,
              "the value fabric's executed steps ride as `trace` events");
        check("mat/vf-trace-disasm",
              rec.by_kind["trace"][0].body.value("disasm", std::string()) ==
                  "add x2, x0, x1",
              rec.by_kind["trace"][0].body.dump());
        check("mat/vf-carries-df_step",
              rec.by_kind.count("df_step") == 1 &&
                  rec.by_kind["df_step"].size() == 4,
              "one df_step per captured step");
        check("mat/vf-df_step-ops",
              rec.by_kind["df_step"][1].body["ops"].size() == 1,
              rec.by_kind["df_step"][1].body.dump());
        check("mat/vf-carries-df_edge",
              rec.by_kind.count("df_edge") == 1 &&
                  rec.by_kind["df_edge"].size() == 1,
              "the def-use graph rides as df_edge events");
        check("mat/vf-no-regstate", rec.by_kind.count("regstate") == 0,
              "the per-step register RING is x86-64-only; arm64 carries none");

        std::string vtext = recording_to_asmtrace(rec);
        std::istringstream vin(vtext);
        std::string verr;
        auto vreloaded = load_recording(vin, verr);
        check("mat/vf-reloads", vreloaded.has_value(),
              std::string("the value-fabric recording must reload: ") + verr);
        if (vreloaded) {
            check("mat/vf-reload-not-torn", !vreloaded->torn,
                  "a finished arm64 run reloads complete, not torn");
            check("mat/vf-reload-df_step",
                  vreloaded->by_kind.count("df_step") == 1 &&
                      vreloaded->by_kind["df_step"].size() == 4,
                  "df_step events survive the round-trip");
            check("mat/vf-reload-df_edge",
                  vreloaded->by_kind.count("df_edge") == 1 &&
                      vreloaded->by_kind["df_edge"].size() == 1,
                  "df_edge events survive the round-trip");
            check("mat/vf-reload-arch", vreloaded->arch == "aarch64",
                  vreloaded->arch);
        }

        // A run whose producer never ran (setup failure) carries no fabric
        // events — never an empty/fake trace pretending to be a real one.
        author_valuefabric_t not_ran;
        Recording rec2 =
            author_recording(r2, ASM_ARM64, image2, 0x100000, &not_ran);
        check("mat/vf-unran-carries-no-trace", rec2.by_kind.count("trace") == 0,
              "a producer that never ran must not fabricate an empty stream");
    }

    if (failures) {
        std::fprintf(stderr, "%d author-vm check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_author_vm: all checks passed\n");
    return 0;
}
