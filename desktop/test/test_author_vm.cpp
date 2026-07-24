// test_author_vm.cpp — the Author door's three rules
// (docs/internal/gui/06-doors-and-learning.md T5). Synthetic inputs only: no
// Keystone, no Unicorn, no display — so the rules are pinned on every host.
#include <cstdio>
#include <cstring>
#include <string>

#include "author_vm.h"

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
        for (int a : {ASM_ARM64, ASM_RISCV64, ASM_ARM32}) {
            const author_arch_row *row = author_arch(a);
            check(std::string("v1 does not run ") + row->name, !row->can_run,
                  "");
            check(std::string(row->name) + " states the limit on its row",
                  std::string(row->note).find("x86-64-only in v1") !=
                      std::string::npos,
                  row->note);
        }
        // A non-runnable arch still assembles, still shows bytes, and carries
        // the labelled limit — never a silent no-op.
        author_result_t a = author_from_assemble(&ok);
        author_apply_run(a, ASM_ARM64, nullptr, "", false);
        check("an arm64 source still shows its bytes",
              a.hexdump == "48 89 f8 c3", a.hexdump);
        check("...is not marked as run", !a.ran, "");
        check("...and carries the limit note",
              a.limit_note == std::string(kAuthorArchLimit), a.limit_note);
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

    if (failures) {
        std::fprintf(stderr, "%d author-vm check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_author_vm: all checks passed\n");
    return 0;
}
