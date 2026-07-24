// author_vm.h — the Author door's pure view-model
// (docs/internal/gui/06-doors-and-learning.md T5).
//
// The Author door is the pedagogy: type assembly, run it, and see faults as
// DATA. Everything decidable about that lives here, so it is assertable without
// Keystone, without Unicorn and without a display; `ui/author_door.cpp` is the
// widgets and the two engine calls.
//
// THREE RULES, all tested:
//
//  1. **The assembler's diagnostic survives VERBATIM.** `asm_result_t.err`
//     already names the common trap ("assembler skipped N of M statements
//     (check the syntax argument)" — AT&T text under the Intel default). A door
//     that rewrote it into "assembly failed" would delete the only sentence
//     that tells the user what to do.
//  2. **A fault is a CARD, not an error.** The emulator turned a crash into
//     data; the door renders kind + address + registers rather than a toast.
//  3. **v1 runs the x86-64 guest only, and says so per-arch.** The other three
//     arches assemble and show bytes with a labelled limit — never a greyed
//     button with no explanation, and never a silent wrong run.
#ifndef ASMDESK_AUTHOR_VM_H
#define ASMDESK_AUTHOR_VM_H

#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include "asmtest_assemble.h"
#include "asmtest_emu.h"
}

namespace asmdesk {

// One row of the arch-gating table. `can_run` is v1's honest limit, not a
// capability probe: the valtrace/replay tier is x86-64-guest-only.
struct author_arch_row {
    int arch = 0; // asm_arch_t
    const char *name = "";
    bool can_run = false;
    const char *note = "";
};
const std::vector<author_arch_row> &author_arch_table();
const author_arch_row *author_arch(int arch);

// The five dialects, in the header's order; Intel is the default (and the
// source of the loud-drop trap the error strip explains).
struct author_syntax_row {
    int syntax = 0; // asm_syntax_t
    const char *name = "";
};
const std::vector<author_syntax_row> &author_syntax_table();

// What one Run produced. Every string here is either the library's own or one
// of the two constants below — the door invents none of it.
struct author_result_t {
    bool assembled = false;
    std::string asm_error; // VERBATIM asm_result_t.err; "" on success
    size_t bytes = 0;
    std::string hexdump; // "48 89 f8 c3" — always shown, even when not run

    bool ran = false;
    std::string limit_note; // "" unless the arch cannot run in v1

    bool faulted = false;
    std::string fault_kind; // "read" | "write" | "fetch"
    uint64_t fault_addr = 0;
    std::string fault_note; // kLoomAuthorFaultCopy
    std::string disasm;     // the faulting instruction, or "" (D10)
    uint64_t rip = 0, rax = 0, rbx = 0, rcx = 0, rdx = 0, rsi = 0, rdi = 0;
    bool capped = false; // the max_insns guard stopped a spin
};

// Map an assemble result into the view-model. `bytes`/`len` are the assembled
// image (ignored when !r->ok).
author_result_t author_from_assemble(const asm_result_t *r);
// Fold a run's outcome into an already-assembled result. `disasm` may be "".
void author_apply_run(author_result_t &out, int arch, const emu_result_t *res,
                      const std::string &disasm, bool capped);

std::string author_dump(const author_result_t &r);

// Copy pinned verbatim by test_author_vm.cpp.
extern const char *const kAuthorFaultCopy;
extern const char *const kAuthorArchLimit;
extern const char *const kAuthorRenderOnly;

// v1's instruction cap: a spin in the box must not hang the UI, and the cap is
// REPORTED when it fires rather than looking like a clean finish.
inline constexpr uint64_t kAuthorMaxInsns = 1u << 20;

} // namespace asmdesk
#endif // ASMDESK_AUTHOR_VM_H
