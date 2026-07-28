// test_asm_language.cpp — the Author editor's assembly syntax highlighting
// (17-interaction-testing-and-editor.md T2 follow-on, ui/asm_language.h).
//
// The claim under test is NOT "the source looks nicer". It is that the
// highlighter AGREES WITH THE ASSEMBLER about what a character means, because a
// highlighter that disagrees paints a lie about what the next Run will do. The
// two ambiguous characters carry the whole risk:
//
//   ';'  an end-of-line COMMENT under NASM, a statement SEPARATOR under
//        Intel / AT&T / MASM / GAS.
//   '#'  a comment on x86 and RISC-V, the IMMEDIATE PREFIX on ARM — where
//        treating it as a comment would grey out the operand of `mov x0, #1`.
//
// Both are `stmt_rules()` in src/assemble.c, and both are asserted here twice
// over: once against the mirrored rule table, and once by driving the REAL
// colorizer and reading the result back. SetText colorizes eagerly, so that
// second half runs headless with no display, no Keystone and no Unicorn.
//
// Reading the colours back: TextEditor exposes IterateIdentifiers, which yields
// exactly the runs the colorizer classed `identifier` or `knownIdentifier`. A
// word therefore appears iff it was NOT swallowed by a comment AND was NOT
// matched as a mnemonic — which is enough to pin every case below.
#include <cstdio>
#include <string>
#include <vector>

#include "imgui.h"

#include "TextEditor.h"
#include "ui/asm_language.h"

using asmdesk::dt_asm_comment_rules;
using asmdesk::dt_asm_comment_rules_for;
using asmdesk::dt_asm_language;

static int fails;
static void check(const char *what, bool cond) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s\n", what);
        fails++;
    }
}

// Colorize `src` under one (arch, dialect) and return every run the colorizer
// classed as an identifier. SetLanguage BEFORE SetText: setText is what runs the
// colorizer over the whole document.
static std::vector<std::string> idents(int arch, int syntax,
                                       const std::string &src) {
    TextEditor ed;
    ed.SetLanguage(dt_asm_language(arch, syntax));
    ed.SetText(src);
    std::vector<std::string> out;
    ed.IterateIdentifiers([&out](const std::string &s) { out.push_back(s); });
    return out;
}

static bool has(const std::vector<std::string> &v, const char *word) {
    for (const std::string &s : v)
        if (s == word)
            return true;
    return false;
}

// Every (arch, dialect) pair the Author door can put the editor in.
struct combo {
    int arch;
    int syntax;
    const char *label;
};
static const combo kCombos[] = {
    {ASM_X86_64, ASM_SYNTAX_INTEL, "x86/Intel"},
    {ASM_X86_64, ASM_SYNTAX_ATT, "x86/AT&T"},
    {ASM_X86_64, ASM_SYNTAX_NASM, "x86/NASM"},
    {ASM_X86_64, ASM_SYNTAX_MASM, "x86/MASM"},
    {ASM_X86_64, ASM_SYNTAX_GAS, "x86/GAS"},
    {ASM_ARM64, ASM_SYNTAX_INTEL, "arm64"},
    {ASM_ARM32, ASM_SYNTAX_INTEL, "arm32"},
    {ASM_RISCV64, ASM_SYNTAX_INTEL, "riscv64"},
};

int main() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    // --- 1. every combo yields a definition, and it is never null ------------
    for (const combo &c : kCombos) {
        const TextEditor::Language *l = dt_asm_language(c.arch, c.syntax);
        check("a language exists for the combo", l != nullptr);
        if (l == nullptr)
            continue;
        check("the definition is named", !l->name.empty());
        check("mnemonics are case-insensitive", !l->caseSensitive);
        check("'.' opens a directive line", l->preprocess == '.');
        check("'//' comments in every dialect", l->singleLineCommentAlt == "//");
        check("slash-star comments in every dialect", l->commentStart == "/*");
        // A lone `'` is legal GAS (`movb $'a, %al`) and colorizer string state
        // survives line ends, so honouring it would paint the rest of the file.
        check("single-quoted strings stay off", !l->hasSingleQuotedStrings);
        check("a tokenizer set is installed",
              l->isPunctuation && l->getIdentifier && l->getNumber);
    }
    // An unknown arch must still highlight rather than fall back to flat text.
    check("an unknown arch falls back, never to null",
          dt_asm_language(999, 999) != nullptr);

    // --- 2. the definitions MIRROR src/assemble.c's stmt_rules() -------------
    // This is the invariant that keeps the editor and the assembler in step: the
    // comment character the colorizer uses is exactly the one the rule table
    // says introduces a comment, and nothing else does.
    for (const combo &c : kCombos) {
        const dt_asm_comment_rules r = dt_asm_comment_rules_for(c.arch, c.syntax);
        const std::string &lc = dt_asm_language(c.arch, c.syntax)->singleLineComment;
        check("';' comments iff the rule table says so",
              (lc == ";") == r.semi_is_comment);
        check("'#' comments iff the rule table says so",
              (lc == "#") == r.hash_is_comment);
        check("no third introducer appears",
              lc == ";" || lc == "#" || lc.empty());
        check("';' and '#' are never both comments",
              !(r.semi_is_comment && r.hash_is_comment));
    }
    // Spelled out, so a silent flip in either direction is a failure and not a
    // tautology against a table that moved with it.
    check("NASM is the ONLY dialect where ';' comments",
          dt_asm_comment_rules_for(ASM_X86_64, ASM_SYNTAX_NASM).semi_is_comment &&
              !dt_asm_comment_rules_for(ASM_X86_64, ASM_SYNTAX_INTEL)
                   .semi_is_comment &&
              !dt_asm_comment_rules_for(ASM_X86_64, ASM_SYNTAX_ATT)
                   .semi_is_comment &&
              !dt_asm_comment_rules_for(ASM_X86_64, ASM_SYNTAX_MASM)
                   .semi_is_comment &&
              !dt_asm_comment_rules_for(ASM_X86_64, ASM_SYNTAX_GAS)
                   .semi_is_comment &&
              !dt_asm_comment_rules_for(ASM_ARM64, ASM_SYNTAX_INTEL)
                   .semi_is_comment);
    check("'#' never comments on ARM (it is the immediate prefix)",
          !dt_asm_comment_rules_for(ASM_ARM64, ASM_SYNTAX_INTEL).hash_is_comment &&
              !dt_asm_comment_rules_for(ASM_ARM32, ASM_SYNTAX_INTEL)
                   .hash_is_comment);
    check("'#' comments on x86 (non-NASM) and RISC-V",
          dt_asm_comment_rules_for(ASM_X86_64, ASM_SYNTAX_INTEL).hash_is_comment &&
              dt_asm_comment_rules_for(ASM_X86_64, ASM_SYNTAX_ATT)
                  .hash_is_comment &&
              dt_asm_comment_rules_for(ASM_RISCV64, ASM_SYNTAX_INTEL)
                  .hash_is_comment);

    // --- 3. mnemonics and registers land in DIFFERENT colour classes ---------
    {
        const TextEditor::Language *x86 =
            dt_asm_language(ASM_X86_64, ASM_SYNTAX_INTEL);
        check("a mnemonic is a keyword", x86->keywords.count("mov") == 1);
        check("a mnemonic is not also a register",
              x86->identifiers.count("mov") == 0);
        check("a register is a known identifier",
              x86->identifiers.count("rax") == 1);
        check("a register is not also a mnemonic",
              x86->keywords.count("rax") == 0);
        check("the numbered register family is generated",
              x86->identifiers.count("r15") == 1 &&
                  x86->identifiers.count("r15d") == 1 &&
                  x86->identifiers.count("xmm0") == 1);
        check("GAS operand suffixes are mnemonics",
              x86->keywords.count("movq") == 1 &&
                  x86->keywords.count("movslq") == 1);
        check("the condition-code families are generated",
              x86->keywords.count("jne") == 1 &&
                  x86->keywords.count("sete") == 1 &&
                  x86->keywords.count("cmovae") == 1);
        check("Intel operand decorators are declarations",
              x86->declarations.count("qword") == 1 &&
                  x86->declarations.count("ptr") == 1 &&
                  x86->declarations.count("lock") == 1);

        const TextEditor::Language *a64 =
            dt_asm_language(ASM_ARM64, ASM_SYNTAX_INTEL);
        check("AArch64 mnemonics and registers",
              a64->keywords.count("ldp") == 1 && a64->keywords.count("b.eq") == 1 &&
                  a64->identifiers.count("x0") == 1 &&
                  a64->identifiers.count("xzr") == 1);
        const TextEditor::Language *rv =
            dt_asm_language(ASM_RISCV64, ASM_SYNTAX_INTEL);
        check("RISC-V mnemonics and ABI register names",
              rv->keywords.count("addiw") == 1 && rv->keywords.count("jalr") == 1 &&
                  rv->identifiers.count("a0") == 1 &&
                  rv->identifiers.count("ra") == 1);
        const TextEditor::Language *a32 =
            dt_asm_language(ASM_ARM32, ASM_SYNTAX_INTEL);
        check("ARM32 condition suffixes are generated",
              a32->keywords.count("bne") == 1 && a32->keywords.count("moveq") == 1 &&
                  a32->identifiers.count("r7") == 1);
    }

    // --- 4. the colorizer, driven for real ----------------------------------
    // `zzsentinel` is deliberately not a mnemonic and not a register: it shows up
    // as an identifier exactly when it was NOT swallowed by a comment.
    {
        // ';' under Intel SEPARATES, so what follows it is still code.
        std::vector<std::string> v =
            idents(ASM_X86_64, ASM_SYNTAX_INTEL, "mov rax, 1 ; zzsentinel\n");
        check("Intel: ';' does not start a comment", has(v, "zzsentinel"));
        check("Intel: a register colours as an identifier", has(v, "rax"));
        check("Intel: a mnemonic does NOT (it is a keyword)", !has(v, "mov"));

        // ...and under NASM the same line is half comment.
        v = idents(ASM_X86_64, ASM_SYNTAX_NASM, "mov rax, 1 ; zzsentinel\n");
        check("NASM: ';' starts a comment", !has(v, "zzsentinel"));
        check("NASM: the code before it survives", has(v, "rax"));

        // '#' is a comment on x86...
        v = idents(ASM_X86_64, ASM_SYNTAX_ATT, "movq $1, %rax # zzsentinel\n");
        check("AT&T: '#' starts a comment", !has(v, "zzsentinel"));
        check("AT&T: '%' is a sigil, so ONE register set serves both syntaxes",
              has(v, "rax"));
        check("AT&T: the suffixed mnemonic is a keyword", !has(v, "movq"));

        // ...but on ARM it introduces an immediate, and eating the rest of the
        // line would grey out the operand of every constant load.
        v = idents(ASM_ARM64, ASM_SYNTAX_INTEL, "mov x0, #zzsentinel\n");
        check("AArch64: '#' does NOT start a comment", has(v, "zzsentinel"));
        check("AArch64: the register survives", has(v, "x0"));
        v = idents(ASM_ARM32, ASM_SYNTAX_INTEL, "mov r0, #zzsentinel\n");
        check("ARM32: '#' does NOT start a comment", has(v, "zzsentinel"));

        // '#' IS a comment on RISC-V.
        v = idents(ASM_RISCV64, ASM_SYNTAX_INTEL, "addi a0, a0, 1 # zzsentinel\n");
        check("RISC-V: '#' starts a comment", !has(v, "zzsentinel"));
        check("RISC-V: the ABI register survives", has(v, "a0"));

        // '//' is LLVM-wide: it comments in EVERY dialect, including the two
        // where neither ';' nor '#' does.
        for (const combo &c : kCombos) {
            v = idents(c.arch, c.syntax, "nop // zzsentinel\n");
            check("'//' comments in every dialect", !has(v, "zzsentinel"));
        }
        for (const combo &c : kCombos) {
            v = idents(c.arch, c.syntax, "nop /* zzsentinel */\nnop zzafter\n");
            check("slash-star comments in every dialect", !has(v, "zzsentinel"));
            check("and it closes rather than running away", has(v, "zzafter"));
        }
    }

    // --- 5. numbers are numbers, not identifiers ----------------------------
    // A tokenizer that gave up after the `0` would leave `x10` looking like a
    // register, which is the exact confusion this door exists to prevent.
    {
        std::vector<std::string> v = idents(
            ASM_X86_64, ASM_SYNTAX_INTEL,
            "mov rax, 0x10\nmov rbx, 0b1010\nmov rcx, 0Ah\nmov rdx, 42\n");
        check("hex does not leave an 'x10' identifier behind", !has(v, "x10"));
        check("binary does not leave a 'b1010' identifier behind",
              !has(v, "b1010"));
        check("the MASM trailing-h form is one number", !has(v, "Ah") &&
                                                            !has(v, "h"));
        check("the registers around them still read", has(v, "rax") &&
                                                          has(v, "rdx"));
    }

    // --- 6. a directive line is a directive line ----------------------------
    {
        std::vector<std::string> v = idents(ASM_X86_64, ASM_SYNTAX_ATT,
                                            ".globl zzsentinel\nmovq $1, %rax\n");
        check("a line opening with '.' colours whole as a directive",
              !has(v, "zzsentinel"));
        check("the instruction line after it is unaffected", has(v, "rax"));
        // Mid-line, '.' is just part of a label reference.
        v = idents(ASM_X86_64, ASM_SYNTAX_ATT, "jmp .Lzzsentinel\n");
        check("a mid-line label keeps its leading dot in one token",
              has(v, ".Lzzsentinel"));
    }

    // --- 7. an unclosed GAS character literal does not eat the file ---------
    {
        std::vector<std::string> v =
            idents(ASM_X86_64, ASM_SYNTAX_ATT, "movb $'a, %al\nmovq $2, %rbx\n");
        check("a lone ' does not swallow the following lines", has(v, "rbx"));
    }

    // --- 8. case folds ------------------------------------------------------
    {
        std::vector<std::string> v =
            idents(ASM_X86_64, ASM_SYNTAX_INTEL, "MOV RAX, 1\n");
        check("an upper-case mnemonic is still a keyword", !has(v, "MOV"));
        check("an upper-case register is still a register", has(v, "RAX"));
    }

    ImGui::DestroyContext();
    if (fails) {
        std::fprintf(stderr, "test_asm_language: %d FAILURE(S)\n", fails);
        return 1;
    }
    std::printf("test_asm_language: all checks passed\n");
    return 0;
}
