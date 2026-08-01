// asm_language.h — assembly syntax highlighting for the Author door's editor
// (docs/internal/archive/gui/17-interaction-testing-and-editor.md T2, the noted follow-on).
//
// goossens ImGuiColorTextEdit ships language definitions for C/C++/Lua/SQL/… but
// none for assembly, so the Author door rendered its source as flat text. A
// `TextEditor::Language` is a plain struct of rules, so the definition is ours to
// fill in — this file builds one per (arch, dialect) and hands it to
// `editor.SetLanguage()`.
//
// **THE RULE THAT MATTERS.** A highlighter that disagrees with the assembler is
// worse than none: it paints a lie about what the next Run will do. ';' is the
// case that bites — it is an end-of-line COMMENT under NASM but a statement
// SEPARATOR under Intel/AT&T/MASM/GAS, and '#' is a comment on x86 but the
// IMMEDIATE prefix on ARM (`mov x0, #1`). Those are not guesses: they are
// `stmt_rules()` in src/assemble.c, the table the statement counter already
// derives from Keystone's behaviour. `dt_asm_comment_rules_for` mirrors it
// exactly, and test_asm_language pins the mirror — so a dialect whose rule
// changes there breaks the test here rather than silently miscolouring.
//
// No engine call and no display: this is data plus three tokenizer callbacks, so
// it compiles into the app, the render-only viewer and the null-backend test
// tree alike, and every claim it makes is asserted headlessly.
#ifndef ASMDESK_UI_ASM_LANGUAGE_H
#define ASMDESK_UI_ASM_LANGUAGE_H

#include "TextEditor.h"

extern "C" {
#include "asmtest_assemble.h" // asm_arch_t / asm_syntax_t, as ints
}

namespace asmdesk {

// How one (arch, dialect) treats the two ambiguous introducers. MIRRORS
// `stmt_rules()` in src/assemble.c — see the header comment; '//' and slash-star
// are LLVM-wide and so apply to every dialect unconditionally.
struct dt_asm_comment_rules {
    bool semi_is_comment = false; // ';' starts a comment rather than separating
    bool hash_is_comment = true;  // '#' starts a comment rather than being data
};
dt_asm_comment_rules dt_asm_comment_rules_for(int arch, int syntax);

// The language definition for one (arch, dialect). Never null: an unknown arch
// falls back to the x86-64 Intel definition, because a flat-text editor is a
// worse answer than a slightly-wrong-mnemonic-set one. `syntax` is honoured for
// x86-64 only, matching asmtest_assemble.h (it is ignored for the other guests).
//
// Colour classes, and what each carries:
//   keyword          mnemonics (`mov`, `bl`, `csrrw`)
//   declaration      prefixes and operand decorators (`lock`, `rep`, `qword ptr`)
//   knownIdentifier  registers (`rax`, `x0`, `a0`) — including after AT&T's '%'
//   preprocessor     a whole line starting with '.' (`.text`, `.globl`, `.L1:`)
//   number           0x… / 0b… / decimal / MASM's trailing-h form
//   comment          per the rules above
const TextEditor::Language *dt_asm_language(int arch, int syntax);

} // namespace asmdesk
#endif // ASMDESK_UI_ASM_LANGUAGE_H
