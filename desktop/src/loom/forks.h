// forks.h — one-fact takes (05-loom-day-one.md T5). FULL BUILD ONLY.
//
// A "take" is the Loom's counterfactual: change exactly ONE fact — an entry
// argument, or the routine's source — re-run from entry, and weave the result
// beside the base fabric. That is the whole mechanic; everything interesting
// about it is what it refuses to do.
//
// LICENSING (D4/D9). This TU is the only part of loom/ that touches the C
// library's engine tiers (asmtest_assemble -> Keystone, emu_* -> Unicorn,
// asmtest_dataflow_emu_run -> Unicorn). It compiles ONLY into the full
// `asmtest-desktop` binary, which is GPL-2.0 as a whole; `asmtest-viewer`
// never sees this file and stays engine-free and permissively distributable.
//
// FORKS NEVER TOUCH A LIVE PROCESS. A take is an emulator replay, an explicit
// crossing of the native->virtual line — never evidence about a live process or
// about silicon timing. `kLoomForkDisclosure` is that sentence, and the panel
// shows it.
//
// DETERMINISM IS A TEST SUBJECT, not a hope, and it rests on two facts verified
// in the producer:
//   * asmtest_dataflow_emu_run opens its OWN uc_engine per call and zeroes the
//     guest GP file (src/dataflow_emu.c:257, :229) — every fabric run is
//     hermetic;
//   * emu_* mapped memory deliberately PERSISTS across emu_call_* (the caller's
//     preload mechanism, asmtest_emu.h:586), so the fault-card run must be
//     bracketed by emu_snapshot/emu_restore or take N inherits take N-1's dirt.
// The bracket is not optional and `loom_take_run` does it unconditionally.
#ifndef ASMDESK_LOOM_FORKS_H
#define ASMDESK_LOOM_FORKS_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "asmtest_emu.h"
#include "loom/fabric.h"

namespace asmdesk {

extern const char *const kLoomForkDisclosure;

struct loom_edit_t {
    enum class kind { entry_arg, code_patch };
    kind k = kind::entry_arg;
    int arg_index = 0;          // entry_arg
    long arg_value = 0;         // entry_arg
    std::string patched_source; // code_patch: the FULL listing, never a diff
    int syntax = 0;             // code_patch: asm_syntax_t
    std::string describe() const;
};

// One take's result. It owns its buffers; `vt`/`g` are the C views the fabric
// builder consumes, re-pointed by `bind()` (so the struct survives a move).
struct loom_take_t {
    loom_edit_t edit;
    std::vector<uint8_t> code; // the bytes that actually ran
    std::vector<long> args;    // the arguments that actually ran

    std::vector<uint64_t> insn_off;
    std::vector<at_val_rec_t> recs;
    std::vector<asmtest_defuse_edge_t> edges;

    emu_result_t result{};
    std::vector<uint64_t> trace_insns; // emu_call_traced's ordered offsets
    bool trace_truncated = false;
    // LOUD, never silent: a take that could not be built says why, and the
    // caller renders this verbatim rather than an empty fabric.
    std::string err;

    const asmtest_valtrace_t *vt() const;
    const asmtest_defuse_t *g() const;
    loom_provenance_t provenance() const;
    // The fault card's text, or "" when the take did not fault.
    std::string fault_card() const;

  private:
    mutable asmtest_valtrace_t vt_{};
    mutable asmtest_defuse_t g_{};
};

// Apply exactly one edit and re-run from entry.
//
// `session` + `base_state` are the app's shared emulator handle and the
// snapshot taken when the fork session opened; every take is bracketed by
// emu_restore(session, base_state) before it runs. Both may be null, in which
// case the fault-card leg is skipped and the take carries only the value
// fabric — stated in `out->err` rather than silently omitted.
//
// Returns false with `out->err` set on: an unassemblable patch (the loud-drop
// contract turns a silently skipped statement into a visible fork failure —
// never a fabric of code the user did not write), an out-of-range arg index, or
// a producer setup failure.
bool loom_take_run(emu_t *session, const emu_snapshot_t *base_state,
                   const uint8_t *code, size_t code_len, const long *args,
                   int nargs, const loom_edit_t &edit, loom_take_t *out);

} // namespace asmdesk
#endif // ASMDESK_LOOM_FORKS_H
