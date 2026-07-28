// author_vm.h — the Author door's pure view-model
// (docs/internal/gui/06-doors-and-learning.md T5).
//
// The Author door is the pedagogy: type assembly, run it, and see faults as
// DATA. Everything decidable about that lives here, so it is assertable without
// Keystone, without Unicorn and without a display; `ui/author_door.cpp` is the
// widgets and the engine calls.
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
//  3. **v1 runs the x86-64 AND arm64 guests, each honestly, and says so
//     per-arch.** The remaining two arches assemble and show bytes with a
//     labelled limit — never a greyed button with no explanation, and never a
//     silent wrong run. The two runnable guests do not share a result SHAPE:
//     x86-64 runs through `emu_call_traced` (an `emu_result_t` register/fault
//     snapshot); arm64 runs through the per-guest value-fabric producer
//     (`src/dataflow_emu.c`, 32-per-guest-value-producer.md R5 T3) — def-use
//     edges + per-step operand values, with NO register file and NO fault
//     kind/address to show. The door renders each shape as what it actually is
//     (`author_result_t::ran` vs `::ran_value_fabric`) rather than forcing one
//     onto the other (D7).
#ifndef ASMDESK_AUTHOR_VM_H
#define ASMDESK_AUTHOR_VM_H

#include <cstdint>
#include <string>
#include <vector>

#include "doc/recording.h" // author_recording materialises a Recording (T3)

extern "C" {
#include "asmtest_assemble.h"
#include "asmtest_emu.h"
}
// at_val_rec_t / asmtest_defuse_edge_t (R5 T3 value fabric). Plain structs —
// self-wrapped in extern "C" already — so including this adds no Capstone/
// Unicorn linkage to this pure header, exactly like asmtest_emu.h above.
#include "asmtest_valtrace.h"

namespace asmdesk {

// One row of the arch-gating table. `can_run` is v1's honest limit, not a
// capability probe: it names whether THIS build's v1 runs the arch at all, via
// whichever of the two run paths above applies — never the same thing for
// every runnable row.
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

    // R5 T3 (32-per-guest-value-producer.md): a per-guest VALUE FABRIC run
    // (arm64 in v1) — def-use edges + per-step operand values, never an
    // emu_result_t register/fault snapshot. `ran` above STAYS false for this
    // path: it means "the emu_result_t fields above (rip/rax/…, fault_kind,
    // fault_addr) are populated", which they are not here, and rendering them
    // anyway (as zeros) would be a D7 lie by omission.
    bool ran_value_fabric = false;
    bool vf_ok = false; // the guest reached the return sentinel cleanly
    size_t vf_steps = 0;
    size_t vf_edges = 0;
    std::string vf_note; // the honest capability/degrade note; always set
};

// Map an assemble result into the view-model. `bytes`/`len` are the assembled
// image (ignored when !r->ok).
author_result_t author_from_assemble(const asm_result_t *r);
// Fold a run's outcome into an already-assembled result. `disasm` may be "".
void author_apply_run(author_result_t &out, int arch, const emu_result_t *res,
                      const std::string &disasm, bool capped);

// A per-guest VALUE FABRIC run's result (32-per-guest-value-producer.md R5
// T3): the arm64 (and any future non-x86-64) guest runs through the emulator
// L0 producer (src/dataflow_emu.c's asmtest_dataflow_emu_run_arch), which
// yields def-use edges + per-step operand values. Plain data — no engine call
// lives here — gathered by ui/author_door.cpp (which DOES call the producer)
// so this header stays as engine-free as the rest of the view-model, and so
// author_recording can serialize it through the SAME writer the codeimage
// event already uses (recording_to_asmtrace), rather than a new
// serialization.
struct author_valuefabric_t {
    bool ran = false; // the producer ran at all (false only on setup failure)
    bool ok = false;  // the guest reached the return sentinel cleanly
    bool truncated = false; // a value-trace buffer filled: a lower bound (D7)
    std::vector<uint64_t> insn_off;  // per step, offset from the routine entry
    std::vector<std::string> disasm; // parallel to insn_off; "" when none (D10)
    std::vector<at_val_rec_t> recs;  // ALL steps' operand records, by step
    std::vector<asmtest_defuse_edge_t> edges; // the L1 def-use graph
    std::vector<uint8_t> wide; // side buffer for >8-byte operand values
};

// Fold a value-fabric run's outcome into an already-assembled result (the
// arm64 counterpart of author_apply_run, which stays the x86-64 path
// unchanged). Honest by construction (D7): it never fabricates a fault
// kind/address the producer did not report, and it says so plainly when the
// guest did not reach a clean stop or a buffer truncated the fabric.
void author_apply_run_vf(author_result_t &out, const author_valuefabric_t &vf);

std::string author_dump(const author_result_t &r);

// Materialise an authored run into a Recording (18-breach-stops.md T3, F24), so
// the SAME save_recording_file + confirm-overwrite dialog the Inspect door uses
// applies unchanged and Author output stops being ephemeral. The recording
// carries exact author-emulator provenance and — when `image` is non-empty — the
// assembled program as a `codeimage` event (the bytes a reader needs to reopen
// what was authored). When `vf` is non-null and it ran (R5 T3), its steps/
// operands/def-use edges ride as `trace` + `df_step` + `df_edge` events too —
// the SAME schema shape tools/asmtrace_record.c's emulator producer writes —
// so an arm64 run opens in the Loom / Slice / Timeline like any other
// recording. Pure struct/JSON assembly, no engine call, so test_author_vm pins
// the round-trip on any host. `base` is the image's load address
// (EMU_CODE_BASE).
Recording author_recording(const author_result_t &r, int arch,
                           const std::vector<uint8_t> &image, uint64_t base,
                           const author_valuefabric_t *vf = nullptr);

// Copy pinned verbatim by test_author_vm.cpp.
extern const char *const kAuthorFaultCopy;
extern const char *const kAuthorArchLimit;
extern const char *const kAuthorRenderOnly;

// v1's instruction cap: a spin in the box must not hang the UI, and the cap is
// REPORTED when it fires rather than looking like a clean finish.
inline constexpr uint64_t kAuthorMaxInsns = 1u << 20;

} // namespace asmdesk
#endif // ASMDESK_AUTHOR_VM_H
