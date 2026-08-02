// test_mnemonic.cpp — the engine-free instruction classifier
// (docs/internal/gui/54-3d-catalog-phase0-plumbing.md T4).
//
// Two halves. The first is representative per-class spot checks. The second
// is the ANTI-DRIFT walk: every real keyword the Author editor's highlighter
// generates (ui/asm_language.cpp, read back through the public
// dt_asm_language API so this file never touches its file-local word-list
// arrays) must classify to something other than Unknown, and the ambiguous
// flag must match this file's own documented set exactly — a mnemonic added
// to asm_language.cpp's lists and not mirrored into space/mnemonic.cpp fails
// a named check here rather than silently under-classifying.
#include <cstdio>
#include <string>
#include <vector>

#include "imgui.h"

#include "TextEditor.h"
#include "space/mnemonic.h"
#include "ui/asm_language.h"

using namespace asmdesk;
using namespace asmdesk::space;

static int failures;
static void fail(const std::string &what, const std::string &why) {
    std::fprintf(stderr, "FAIL %s: %s\n", what.c_str(), why.c_str());
    failures++;
}
static void check(const std::string &what, bool cond, const std::string &why) {
    if (!cond)
        fail(what, why);
}

// The exact ambiguous set this file documents (mnemonic.cpp's header
// comment + per-entry comments): mnemonics that legitimately cross the
// GPR<->vector/float register-file boundary. Every other real keyword must
// NOT be ambiguous.
static bool expect_ambiguous_x86(const std::string &tok) {
    static const std::vector<std::string> kSet = {
        "movd", "movq", "cvtsi2sd", "cvtsi2ss", "cvtsd2si",
        "cvtss2si", "cvttsd2si", "cvttss2si"};
    for (const std::string &s : kSet)
        if (s == tok)
            return true;
    return false;
}
static bool expect_ambiguous_arm64(const std::string &tok) {
    static const std::vector<std::string> kSet = {
        "fmov", "fcvt", "fcvtzs", "fcvtzu", "scvtf",
        "ucvtf", "dup", "ins", "umov", "smov"};
    for (const std::string &s : kSet)
        if (s == tok)
            return true;
    return false;
}

int main() {
    // --- representative spot checks, per class, per guest ------------------
    check("x86 mov -> Move",
          mnemonic_class("mov", "x86").cls == OpClass::Move, "");
    check("x86 MOV case-insensitive",
          mnemonic_class("MOV", "x86").cls == OpClass::Move, "");
    check("x86 add -> IntArith",
          mnemonic_class("add", "x86").cls == OpClass::IntArith, "");
    check("x86 addl (suffixed) -> IntArith",
          mnemonic_class("addl", "x86").cls == OpClass::IntArith, "");
    check("x86 xor -> Logic",
          mnemonic_class("xor", "x86").cls == OpClass::Logic, "");
    check("x86 shrq (suffixed) -> Logic",
          mnemonic_class("shrq", "x86").cls == OpClass::Logic, "");
    check("x86 cmp -> CompareBranch",
          mnemonic_class("cmp", "x86").cls == OpClass::CompareBranch, "");
    check("x86 jne (jcc) -> CompareBranch",
          mnemonic_class("jne", "x86").cls == OpClass::CompareBranch, "");
    check("x86 setne (setcc) -> CompareBranch",
          mnemonic_class("setne", "x86").cls == OpClass::CompareBranch, "");
    check("x86 cmovne (cmovcc) -> Move",
          mnemonic_class("cmovne", "x86").cls == OpClass::Move, "");
    check("x86 callq (suffixed) -> CompareBranch",
          mnemonic_class("callq", "x86").cls == OpClass::CompareBranch, "");
    check("x86 addss -> ScalarFloat",
          mnemonic_class("addss", "x86").cls == OpClass::ScalarFloat, "");
    check("x86 addps -> VectorSIMD",
          mnemonic_class("addps", "x86").cls == OpClass::VectorSIMD, "");
    check("x86 syscall -> System",
          mnemonic_class("syscall", "x86").cls == OpClass::System, "");
    check("x86 movd ambiguous", mnemonic_class("movd", "x86").ambiguous, "");
    check("x86 mov NOT ambiguous", !mnemonic_class("mov", "x86").ambiguous, "");

    check("arm64 mov -> Move",
          mnemonic_class("mov", "arm64").cls == OpClass::Move, "");
    check("arm64 add -> IntArith",
          mnemonic_class("add", "arm64").cls == OpClass::IntArith, "");
    check("arm64 orr -> Logic",
          mnemonic_class("orr", "arm64").cls == OpClass::Logic, "");
    check("arm64 cmp -> CompareBranch",
          mnemonic_class("cmp", "arm64").cls == OpClass::CompareBranch, "");
    check("arm64 b.eq (cond branch) -> CompareBranch",
          mnemonic_class("b.eq", "arm64").cls == OpClass::CompareBranch, "");
    check("arm64 bl -> CompareBranch",
          mnemonic_class("bl", "arm64").cls == OpClass::CompareBranch, "");
    check("arm64 fadd -> ScalarFloat",
          mnemonic_class("fadd", "arm64").cls == OpClass::ScalarFloat, "");
    check("arm64 ld1 -> VectorSIMD",
          mnemonic_class("ld1", "arm64").cls == OpClass::VectorSIMD, "");
    check("arm64 svc -> System",
          mnemonic_class("svc", "arm64").cls == OpClass::System, "");
    check("arm64 fmov ambiguous", mnemonic_class("fmov", "arm64").ambiguous,
          "");

    // --- abstain cases -------------------------------------------------------
    check("unknown token -> Unknown",
          mnemonic_class("frobnicate", "x86").cls == OpClass::Unknown, "");
    check("empty token -> Unknown",
          mnemonic_class("", "x86").cls == OpClass::Unknown, "");
    check("unknown guest -> Unknown",
          mnemonic_class("mov", "riscv").cls == OpClass::Unknown, "");
    check("empty guest -> Unknown", mnemonic_class("mov", "").cls ==
                                        OpClass::Unknown,
          "");

    // --- anti-drift: every REAL keyword classifies to non-Unknown ----------
    const TextEditor::Language *x86 =
        dt_asm_language(ASM_X86_64, ASM_SYNTAX_INTEL);
    check("x86 language resolved", x86 != nullptr,
          "dt_asm_language returned null");
    if (x86 != nullptr) {
        size_t n = 0, amb_seen = 0;
        for (const std::string &kw : x86->keywords) {
            n++;
            MnemonicClass mc = mnemonic_class(kw, "x86");
            check("x86 keyword classifies: " + kw,
                  mc.cls != OpClass::Unknown,
                  "asm_language.cpp has a mnemonic mnemonic.cpp's copy lacks");
            bool want_amb = expect_ambiguous_x86(kw);
            if (want_amb)
                amb_seen++;
            check("x86 ambiguous set matches: " + kw, mc.ambiguous == want_amb,
                  want_amb
                      ? "expected ambiguous, classifier disagreed"
                      : "classifier marked ambiguous outside the documented "
                        "set");
        }
        check("x86 keyword set non-trivial", n > 100,
              "only " + std::to_string(n));
        check("x86 every documented-ambiguous token is real (8)",
              amb_seen == 8, "saw " + std::to_string(amb_seen) + "/8");
    }

    const TextEditor::Language *arm64 =
        dt_asm_language(ASM_ARM64, ASM_SYNTAX_INTEL);
    check("arm64 language resolved", arm64 != nullptr,
          "dt_asm_language returned null");
    if (arm64 != nullptr) {
        size_t n = 0, amb_seen = 0;
        for (const std::string &kw : arm64->keywords) {
            n++;
            MnemonicClass mc = mnemonic_class(kw, "arm64");
            check("arm64 keyword classifies: " + kw,
                  mc.cls != OpClass::Unknown,
                  "asm_language.cpp has a mnemonic mnemonic.cpp's copy lacks");
            bool want_amb = expect_ambiguous_arm64(kw);
            if (want_amb)
                amb_seen++;
            check("arm64 ambiguous set matches: " + kw,
                  mc.ambiguous == want_amb,
                  want_amb
                      ? "expected ambiguous, classifier disagreed"
                      : "classifier marked ambiguous outside the documented "
                        "set");
        }
        check("arm64 keyword set non-trivial", n > 60,
              "only " + std::to_string(n));
        check("arm64 every documented-ambiguous token is real (10)",
              amb_seen == 10, "saw " + std::to_string(amb_seen) + "/10");
    }

    if (failures) {
        std::fprintf(stderr, "%d test_mnemonic check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_mnemonic: all checks passed\n");
    return 0;
}
