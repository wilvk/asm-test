// regsynth.cpp — the emulator-backed register-history synthesiser of regsynth.h.
// FULL BUILD ONLY (engine-linked): it opens an emu_t, arms the per-step register
// ring and re-runs the recorded code, then folds the ring into the SAME StepIndex
// the `--steps` producer emits. Only asmtest-desktop links this; the render-only
// viewer never does (D4).
#include "views/regsynth.h"

#include <cstdint>
#include <vector>

extern "C" {
#include "asmtest_emu.h" // emu_t + emu_step_capture/emu_call/emu_step_at
}

namespace asmdesk {

namespace {

// Decode a lowercase/uppercase hex string ("48 89..." with NO spaces) into bytes.
// false on an odd length or a non-hex nibble — a malformed codeimage, not data.
bool hex_to_bytes(const std::string &h, std::vector<uint8_t> &out) {
    if (h.empty() || (h.size() % 2) != 0)
        return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    out.clear();
    out.reserve(h.size() / 2);
    for (size_t i = 0; i < h.size(); i += 2) {
        int hi = nib(h[i]), lo = nib(h[i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

} // namespace

bool regsynth_synthesize(const Recording &r, StepIndex *out, std::string *err) {
    auto seterr = [&](const std::string &m) {
        if (err != nullptr)
            *err = m;
        return false;
    };
    if (out == nullptr)
        return seterr("regsynth: no output index");
    if (r.arch != "x86_64")
        return seterr("register-history synthesis is x86-64-guest only");

    // The code bytes come from the recording's own codeimage (28 R1 / Author).
    auto it = r.by_kind.find("codeimage");
    if (it == r.by_kind.end() || it->second.empty())
        return seterr("the recording carries no codeimage — nothing to re-run");
    const nlohmann::json &ci = it->second.front().body;
    auto b = ci.find("bytes");
    if (b == ci.end() || !b->is_string())
        return seterr("the codeimage event has no `bytes` — cannot re-run");
    std::vector<uint8_t> code;
    if (!hex_to_bytes(b->get<std::string>(), code) || code.empty())
        return seterr("the codeimage `bytes` are malformed");

    emu_t *e = emu_open();
    if (e == nullptr)
        return seterr("could not open the emulator");
    constexpr size_t kCap = 1u << 16; // generous; truncation flows to the deck
    if (!emu_step_capture(e, kCap)) {
        emu_close(e);
        return seterr("could not arm the per-step register ring (x86-64 only)");
    }
    // The re-derivation SEED: the emulator's deterministic zero-GP + SysV frame,
    // no args (the recording carries none) — a re-derivation from code + seed,
    // not the original run's inputs. The label says exactly this.
    emu_result_t res{};
    emu_call(e, code.data(), code.size(), nullptr, 0, 0, &res);
    const size_t held = emu_step_count(e);
    const uint64_t dropped = emu_step_dropped(e);

    // Fold the ring into `regstate` events IDENTICAL to emit_regstate's shape, so
    // build_step_index yields a StepIndex byte-identical to a real --steps one.
    static const char *const kNames[18] = {
        "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp", "r8",
        "r9",  "r10", "r11", "r12", "r13", "r14", "r15", "rip", "rflags"};
    Recording synth;
    synth.arch = r.arch;
    synth.drops_lost = dropped;
    synth.has_steps_total = true;
    synth.steps_total = dropped + held;
    for (size_t i = 0; i < held; i++) {
        emu_x86_regs_t regs;
        if (!emu_step_at(e, i, nullptr, &regs))
            continue;
        const uint64_t v[18] = {regs.rax, regs.rbx, regs.rcx, regs.rdx,
                                regs.rsi, regs.rdi, regs.rbp, regs.rsp,
                                regs.r8,  regs.r9,  regs.r10, regs.r11,
                                regs.r12, regs.r13, regs.r14, regs.r15,
                                regs.rip, regs.rflags};
        nlohmann::json values;
        for (int k = 0; k < 18; k++)
            values[kNames[k]] = v[k];
        nlohmann::json body;
        body["k"] = "regstate";
        body["desc"] = "emu_x86_regs_t@x86_64/sysv";
        body["values"] = values;
        synth.by_kind["regstate"].push_back(
            Event{"regstate", body, synth.next_seq++});
    }
    emu_close(e);

    if (synth.by_kind["regstate"].empty())
        return seterr("the re-run produced no steps (the routine executed none)");

    *out = build_step_index(synth);
    out->synthesized = true; // D6: a re-derivation, never original capture
    return true;
}

} // namespace asmdesk
