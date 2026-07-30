// author_vm.cpp — the pure Author-door model of author_vm.h. No ImGui, and it
// calls no engine function: every field arrives as an argument.
#include "author_vm.h"

#include <cstdio>

namespace asmdesk {

const char *const kAuthorFaultCopy =
    "the emulator turned this into data; on real hardware this would have been "
    "a crash";
const char *const kAuthorArchLimit =
    "assembled, not run — run/trace is x86-64/AArch64-only in v1";
const char *const kAuthorRenderOnly =
    "Author mode requires the full (GPL-2.0) build";

const std::vector<author_arch_row> &author_arch_table() {
    // v1's real limit: the value/replay tier runs two guests, x86-64 and
    // arm64 (32-per-guest-value-producer.md R5 T3) — NOT the same result
    // shape (see author_vm.h's RULE 3). The remaining two assemble (Keystone
    // handles them) and stop there, with the reason on the row rather than in
    // a footnote.
    static const std::vector<author_arch_row> t = {
        {ASM_X86_64, "x86-64", true, "assembles, runs, records"},
        {ASM_ARM64, "AArch64", true,
         "assembles, runs, records a value fabric (def-use edges + per-step "
         "operand values — no register file, no fault card: that is the "
         "x86-64 path only)"},
        {ASM_RISCV64, "RISC-V 64", false,
         "assembles only where the Keystone build has the RISC-V backend; "
         "run/trace is x86-64/AArch64-only in v1"},
        {ASM_ARM32, "ARM32", false, kAuthorArchLimit},
    };
    return t;
}

const author_arch_row *author_arch(int arch) {
    for (const author_arch_row &r : author_arch_table())
        if (r.arch == arch)
            return &r;
    return nullptr;
}

const std::vector<author_syntax_row> &author_syntax_table() {
    static const std::vector<author_syntax_row> t = {
        {ASM_SYNTAX_INTEL, "Intel (default)"},
        {ASM_SYNTAX_ATT, "AT&T"},
        {ASM_SYNTAX_NASM, "NASM"},
        {ASM_SYNTAX_MASM, "MASM"},
        {ASM_SYNTAX_GAS, "GAS"},
    };
    return t;
}

author_result_t author_from_assemble(const asm_result_t *r) {
    author_result_t out;
    if (r == nullptr)
        return out;
    out.assembled = r->ok;
    if (!r->ok) {
        // VERBATIM. src/assemble.c:239 already names the common trap; a
        // paraphrase here would delete the one sentence that says what to do.
        out.asm_error = r->err;
        return out;
    }
    out.bytes = r->len;
    char b[8];
    for (size_t i = 0; i < r->len; i++) {
        std::snprintf(b, sizeof b, "%s%02x", i ? " " : "", r->bytes[i]);
        out.hexdump += b;
    }
    return out;
}

void author_apply_run(author_result_t &out, int arch, const emu_result_t *res,
                      const std::string &disasm, bool capped) {
    const author_arch_row *row = author_arch(arch);
    if (row == nullptr || !row->can_run) {
        // Not an error and not a greyed button with no explanation: the bytes
        // are real, the run is out of scope for v1, and the row says which.
        out.limit_note = row != nullptr ? row->note : kAuthorArchLimit;
        return;
    }
    if (res == nullptr)
        return;
    out.ran = true;
    out.capped = capped;
    out.rip = res->regs.rip;
    out.rax = res->regs.rax;
    out.rbx = res->regs.rbx;
    out.rcx = res->regs.rcx;
    out.rdx = res->regs.rdx;
    out.rsi = res->regs.rsi;
    out.rdi = res->regs.rdi;
    if (!res->faulted)
        return;
    out.faulted = true;
    out.fault_addr = res->fault_addr;
    out.fault_kind = res->fault_kind == EMU_FAULT_READ    ? "read"
                     : res->fault_kind == EMU_FAULT_WRITE ? "write"
                     : res->fault_kind == EMU_FAULT_FETCH ? "fetch"
                                                          : "unknown";
    out.fault_note = kAuthorFaultCopy;
    out.disasm = disasm; // "" degrades to the offset (D10)
}

void author_apply_run_vf(author_result_t &out, const author_valuefabric_t &vf) {
    out.ran_value_fabric = true;
    if (!vf.ran) {
        // A producer setup failure (e.g. could not allocate the value trace).
        // Named, not silently empty (D7) — mirrors author_run's own
        // "the emulator could not be opened on this host" for the x86-64 path.
        out.vf_note =
            "the value-fabric producer could not be set up for this run";
        return;
    }
    out.vf_ok = vf.ok;
    out.vf_steps = vf.insn_off.size();
    out.vf_edges = vf.edges.size();
    if (!vf.ok) {
        // The guest did not reach the return sentinel (a fault, or the
        // instruction cap). Unlike emu_result_t, this producer reports no
        // fault kind or address — never invent one; name the gap instead.
        out.vf_note =
            "the guest did not reach a clean stop (a fault, or the "
            "instruction cap) — this per-guest producer reports no fault "
            "kind or address, unlike the x86-64 emu_result_t path; the "
            "fabric captured up to the stop is still recorded";
    } else if (vf.truncated) {
        out.vf_note =
            "value fabric only (def-use edges + per-step operand values, "
            "never a register/fault snapshot) — a value-trace buffer "
            "filled, so this fabric is a lower bound";
    } else {
        out.vf_note =
            "value fabric only: def-use edges + per-step operand values — "
            "no register file and no fault card (that is the x86-64 "
            "emu_result_t path only). Save this run to view the fabric in "
            "the Loom / Slice / Timeline.";
    }
}

std::string author_dump(const author_result_t &r) {
    std::string o;
    o += r.assembled ? "assembled " + std::to_string(r.bytes) + " bytes\n"
                     : "assemble FAILED: " + r.asm_error + "\n";
    if (!r.hexdump.empty())
        o += "bytes " + r.hexdump + "\n";
    if (!r.limit_note.empty())
        o += "limit " + r.limit_note + "\n";
    if (r.ran) {
        char b[128];
        std::snprintf(b, sizeof b, "ran rip=0x%llx rax=0x%llx\n",
                      (unsigned long long)r.rip, (unsigned long long)r.rax);
        o += b;
    }
    if (r.capped)
        o += "capped at the instruction limit — the run did NOT finish\n";
    if (r.faulted) {
        char b[128];
        std::snprintf(b, sizeof b, "fault %s at 0x%llx\n", r.fault_kind.c_str(),
                      (unsigned long long)r.fault_addr);
        o += b;
        o += "  " + r.fault_note + "\n";
        if (!r.disasm.empty())
            o += "  " + r.disasm + "\n";
    }
    if (r.ran_value_fabric) {
        char b[128];
        std::snprintf(b, sizeof b,
                      "value fabric: %zu step(s), %zu def-use edge(s)\n",
                      r.vf_steps, r.vf_edges);
        o += b;
        o += "  " + r.vf_note + "\n";
    }
    return o;
}

namespace {
const char *arch_wire_name(int arch) {
    switch (arch) {
    case ASM_X86_64:
        return "x86_64";
    case ASM_ARM64:
        return "aarch64";
    case ASM_RISCV64:
        return "riscv64";
    case ASM_ARM32:
        return "arm";
    }
    return "";
}

// The at_loc_kind_t -> schema `space` token (asmtrace-schema.md), byte-for-byte
// the same mapping cli/asmtrace_ndjson.c's loc_space() makes for the CLI/
// asmspy writer — kept in sync by test_author_vm's round-trip assertions
// rather than by sharing a TU across the GPL/engine boundary.
const char *val_space(at_loc_kind_t k) {
    switch (k) {
    case AT_LOC_REG:
        return "reg";
    case AT_LOC_MEM_ABS:
        return "abs";
    case AT_LOC_MEM_OFF:
        return "off";
    }
    return "reg";
}

// One operand record (df_step.ops[] / df_edge.loc), the same shape op_body()
// in cli/asmtrace_ndjson.c serializes for the CLI recorder — reused here so an
// Author-mode value fabric reads identically to one asmtrace_record.c wrote.
nlohmann::json valrec_to_json(const at_val_rec_t &r,
                              const std::vector<uint8_t> &wide) {
    nlohmann::json j;
    j["space"] = val_space(r.kind);
    j["reg"] = r.reg;
    if (r.kind != AT_LOC_REG) {
        j["base"] = r.base;
        j["index"] = r.index;
        j["scale"] = r.scale;
        j["disp"] = r.disp;
        j["addr"] = r.addr;
    }
    j["size"] = r.size;
    j["write"] = r.is_write;
    j["value_valid"] = r.value_valid;
    if (r.wide) {
        j["wide"] = true;
        // A bounds-checked bytes hex string, exactly op_body()'s degrade: a
        // wide flag with no (or a short) side buffer stays bytes-less [wide]
        // rather than reading past it. arm64 v1 carries no wide register read
        // (src/dataflow_emu.c's DF_GUEST_ARM64 has read_wide == NULL), so this
        // branch is unreached today and future-proofs the next guest that
        // adds one.
        if (r.value_valid && r.size > 0 &&
            (size_t)r.wide_off + r.size <= wide.size()) {
            static const char *hexd = "0123456789abcdef";
            std::string bytes;
            bytes.reserve((size_t)r.size * 2);
            for (uint16_t k = 0; k < r.size; k++) {
                uint8_t b = wide[(size_t)r.wide_off + k];
                bytes += hexd[b >> 4];
                bytes += hexd[b & 0xF];
            }
            j["bytes"] = bytes;
        }
    } else if (r.value_valid) {
        j["value"] = r.value;
    }
    return j;
}
} // namespace

Recording author_recording(const author_result_t &r, int arch,
                           const std::vector<uint8_t> &image, uint64_t base,
                           const author_valuefabric_t *vf) {
    Recording rec;
    rec.version = kAsmtraceMajor;
    rec.arch = arch_wire_name(arch);
    rec.producer.name = "asmtest-author";
    // The emulator is an exact, isolated guest: the authored run observed every
    // step. The provenance is accurate about the backend a --record author run
    // would have written.
    rec.provenance.backend = "author-emulator";
    rec.provenance.exact = true;
    rec.provenance.trust = "exact";
    // The assembled program image, as a `codeimage` event (asmtrace-schema.md),
    // so reopening the saved file shows the bytes that were authored rather than
    // an empty recording. Skipped when a run assembled nothing.
    if (!image.empty()) {
        static const char *hexd = "0123456789abcdef";
        std::string bytes;
        bytes.reserve(image.size() * 2);
        for (uint8_t b : image) {
            bytes += hexd[b >> 4];
            bytes += hexd[b & 0xF];
        }
        nlohmann::json ev;
        ev["k"] = "codeimage";
        ev["base"] = base;
        ev["len"] = image.size();
        ev["version"] = 0;
        ev["when"] = 0;
        ev["bytes"] = bytes;
        rec.by_kind["codeimage"].push_back(Event{"codeimage", ev, rec.next_seq++});
    }
    // R5 T3: a per-guest VALUE FABRIC run (arm64) rides as `trace` + `df_step`
    // + `df_edge` events — the SAME schema shape asmtrace_record.c's emulator
    // producer writes (tools/asmtrace_record.c's record_arm64, the
    // arm64-df-chain golden) — so this recording opens in the Loom / Slice /
    // Timeline through the existing reader with NO desktop change. No
    // `regstate` (the per-step register RING is x86-64-only, src/emu.c); no
    // `mem` / `blame` (opt-in derived streams the Author door does not arm).
    // Faithful and partial when the guest did not reach the sentinel (vf->ok ==
    // false) or a buffer truncated the capture: whatever WAS captured still
    // rides, verbatim, rather than being withheld until a "clean" run.
    if (vf != nullptr && vf->ran) {
        for (size_t s = 0; s < vf->insn_off.size(); s++) {
            nlohmann::json ev;
            ev["k"] = "trace";
            ev["basis"] = "rel";
            ev["kind"] = "insn";
            ev["off"] = vf->insn_off[s];
            if (s < vf->disasm.size() && !vf->disasm[s].empty())
                ev["disasm"] = vf->disasm[s];
            rec.by_kind["trace"].push_back(Event{"trace", ev, rec.next_seq++});
        }
        size_t cur = 0;
        for (size_t s = 0; s < vf->insn_off.size(); s++) {
            nlohmann::json ev;
            ev["k"] = "df_step";
            ev["step"] = (uint32_t)s;
            ev["off"] = vf->insn_off[s];
            // 37: state the region base on the wire (the same `base` the codeimage
            // event above carries), so an Author save resolves its span the way
            // the CLI recorders do rather than diverging. Omitted when base == 0.
            if (base)
                ev["rbase"] = base;
            if (s < vf->disasm.size() && !vf->disasm[s].empty())
                ev["disasm"] = vf->disasm[s];
            nlohmann::json ops = nlohmann::json::array();
            // vf->recs arrives already ordered by step (asmtest_valtrace_append
            // stamps steps ascending); group this step's run without a second
            // pass over the whole vector.
            while (cur < vf->recs.size() && vf->recs[cur].step < s)
                cur++;
            while (cur < vf->recs.size() && vf->recs[cur].step == s) {
                ops.push_back(valrec_to_json(vf->recs[cur], vf->wide));
                cur++;
            }
            ev["ops"] = ops;
            rec.by_kind["df_step"].push_back(
                Event{"df_step", ev, rec.next_seq++});
        }
        for (const asmtest_defuse_edge_t &e : vf->edges) {
            nlohmann::json ev;
            ev["k"] = "df_edge";
            ev["from"] = e.from_step;
            ev["to"] = e.to_step;
            ev["loc"] = valrec_to_json(e.loc, vf->wide);
            rec.by_kind["df_edge"].push_back(
                Event{"df_edge", ev, rec.next_seq++});
        }
    }
    // A complete, closed recording (has_end) so it reloads non-torn — an authored
    // run that finished is not an interrupted capture.
    rec.has_end = true;
    rec.declared_events = rec.event_count();
    (void)r; // the run's registers/fault are shown live; the image is the payload
    return rec;
}

} // namespace asmdesk
