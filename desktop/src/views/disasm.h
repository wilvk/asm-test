// disasm.h — the codeimage-versioned disassembly pane (08-observer-views.md T7).
//
// The problem this exists for is temporal, not textual. A JIT patches, frees and
// REUSES code addresses. Read the bytes at an address after the fact and you get
// whatever is there now; disassemble a trace against those and every listing
// looks plausible and some of them are another method entirely. The producer
// side already solved this (asmtest_codeimage.h: a timestamped code-image
// timeline, the userspace equivalent of PERF_RECORD_TEXT_POKE), and the
// `codeimage` event carries its versions into the recording. This file is the
// consumer's half of the same rule.
//
// So the resolution rule here is the wire contract, not a heuristic:
//
//   bytes for address `a` at trace time `t` = the version containing `a`
//   with the GREATEST `when` <= t.
//
// Never the newest version (that is the bug), never the first (that is the same
// bug in the other direction), and when no version qualifies the bytes are
// UNKNOWN — a pane that fell back to "the next one along" would show the
// succeeding method's code for the preceding method's trace, which is exactly
// the failure the whole mechanism exists to prevent.
//
// And it never re-reads live memory. For a replay the process is gone; for a
// live session the bytes at that address have already been established as the
// wrong ones.
#ifndef ASMDESK_VIEWS_DISASM_H
#define ASMDESK_VIEWS_DISASM_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "doc/recording.h"
#include "views/observer.h"

namespace asmdesk {

// One `codeimage` event: a span's bytes as of a logical timestamp.
struct CodeVersion {
    uint64_t base = 0, len = 0;
    uint64_t version = 0; // 0-based within its span
    uint64_t when = 0;    // asmtest_codeimage_now at capture
    std::vector<uint8_t> bytes;

    bool covers(uint64_t addr) const {
        return addr >= base && addr < base + len;
    }
};

// What a byte lookup produced. `found == false` carries WHY, because the two
// ways to fail are different: no version covers the address at all, or none
// covers it at or before the requested time.
struct CodeBytes {
    bool found = false;
    const CodeVersion *version = nullptr;
    const uint8_t *data = nullptr; // borrowed, into the timeline
    size_t len = 0;                // available from `addr` to the span's end
    std::string why;               // set when !found
};

struct DisasmView {
    std::vector<CodeVersion> versions; // in event order
    ObsChrome chrome;

    // The recorded disassembly text (D10) by offset, from `trace` events — the
    // fallback when no code image was captured, and the only thing a
    // render-only viewer has (it links no Capstone).
    std::map<uint64_t, std::string> recorded_disasm;
    std::string trace_basis; // "rel" | "abs"; needed to place recorded text

    // The measured reason no code image was captured, from a `note` event. A
    // pane with no versions and no reason is indistinguishable from a pane
    // nobody asked to capture, which is why the producer emits one.
    std::string unavailable_reason;

    // Distinct spans, in ascending base order — one "region" per tracked span.
    std::vector<uint64_t> spans() const;
};

DisasmView obs_disasm_build(const Recording &r,
                            const ObsLifecycle *lc = nullptr);

// The client-side `asmtest_codeimage_bytes_at`: bytes live at `addr` as of
// logical time `when`. `when == 0` means "the latest version", mirroring the C
// API's own convention.
CodeBytes obs_disasm_bytes_at(const DisasmView &v, uint64_t addr,
                              uint64_t when);

// One rendered row of the pane. `bytes` is the hex of what was live then; when
// no image covers it the row carries the recorded `disasm` text instead, and
// says which of the two it is — a listing that blurred them would let a
// fallback silently masquerade as a byte-exact reconstruction.
struct DisasmRow {
    uint64_t addr = 0;
    std::string bytes;  // hex from the code image; "" when unavailable
    std::string text;   // the recorded disasm, when there is one
    std::string source; // "codeimage v<N>" | "recorded disasm" | "unknown"
};

// The pane for a list of addresses at one trace time (absolute addresses; the
// recording's `window.base` is added to a "rel"-basis offset by the caller).
std::vector<DisasmRow> obs_disasm_rows(const DisasmView &v,
                                       const std::vector<uint64_t> &addrs,
                                       uint64_t when, size_t bytes_per_row);

std::string obs_disasm_dump(const DisasmView &v);

} // namespace asmdesk
#endif // ASMDESK_VIEWS_DISASM_H
