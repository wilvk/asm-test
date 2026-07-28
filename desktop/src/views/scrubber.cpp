// scrubber.cpp — the pure builder + dump of scrubber.h. No ImGui, no I/O.
#include "views/scrubber.h"

#include <cstdio>

namespace asmdesk {

namespace {

std::string hex(uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

} // namespace

uint64_t dt_scrubber_prev(const StepIndex &idx, uint64_t playhead) {
    uint64_t total = idx.total_steps();
    if (total == 0)
        return 0;
    if (playhead >= total)
        playhead = total - 1;
    return playhead == 0 ? 0 : playhead - 1;
}

uint64_t dt_scrubber_next(const StepIndex &idx, uint64_t playhead) {
    uint64_t total = idx.total_steps();
    if (total == 0)
        return 0;
    if (playhead + 1 >= total)
        return total - 1;
    return playhead + 1;
}

dt_scrubber_replay dt_scrubber_replayable(const Recording &r) {
    dt_scrubber_replay out;
    // Emulator-replayable = the recorded x86-64 code bytes exist AND the run was
    // an exact, isolated emulator guest (an Author run) — so re-running under the
    // emulator ring re-derives THIS recording's semantics, not different ones.
    // Ordered so the reason names the FIRST blocking fact.
    const bool have_code = r.by_kind.count("codeimage") != 0;
    const bool x86 = r.arch == "x86_64";
    const bool author = r.provenance.backend == "author-emulator" ||
                        r.producer.name == "asmtest-author";
    if (!have_code) {
        out.why = "no code bytes to re-run — the recording carries no codeimage, "
                  "so a register history cannot be synthesised from it";
        return out;
    }
    if (!x86) {
        out.why = "the register-history producer is x86-64-guest only; this "
                  "recording's guest is " +
                  (r.arch.empty() ? std::string("unset") : r.arch);
        return out;
    }
    if (!author) {
        out.why = "not an emulator-replayable recording — re-deriving a register "
                  "history is faithful only for an Author/emulator run, and this "
                  "is a " +
                  (r.provenance.backend.empty() ? std::string("live")
                                                : r.provenance.backend) +
                  " capture";
        return out;
    }
    out.replayable = true;
    return out;
}

dt_scrubber dt_scrubber_build(const StepIndex &idx, uint64_t playhead,
                              const dt_scrubber_replay &replay) {
    dt_scrubber s;
    s.desc = idx.desc;

    if (!idx.present()) {
        s.present = false;
        // The producer did not run. Where the recording is emulator-replayable
        // (30 R3 T4), OFFER to synthesise a history by re-running under the ring;
        // otherwise keep the honest refusal and its machine reason. Either way,
        // point at the real producer (`--steps`) and deep-link the docs.
        s.absent_message =
            "no per-step register capture in this recording — the regstate "
            "producer was not run (record with asmtrace_record --steps=<cap>).";
        s.docs = "docs/internal/gui/09-teaching-producers.md";
        if (replay.replayable) {
            s.synthesizable = true;
            s.synth_action = "synthesize register history";
            // MANDATORY honesty (D6): a synthesised deck is a RE-DERIVATION, not
            // the bytes the original run observed.
            s.synth_label =
                "re-derived by re-running the recorded code under the emulator "
                "ring — sound and exact, but NOT the bytes the original run "
                "observed; a re-derivation, not original capture (30 R3 T4)";
        } else {
            // Not replayable here: name why, so the refusal is a fact about this
            // recording, not a blanket never.
            s.absent_message += " " + replay.why + ".";
        }
        return s;
    }

    s.present = true;
    s.synthesized = idx.synthesized;
    s.dropped = idx.dropped;
    s.first_held = idx.first_step;
    s.held = idx.count();
    s.last_held = idx.first_step + idx.count() - 1;
    s.total = idx.total_steps();

    uint64_t ph = playhead;
    if (ph >= s.total)
        ph = s.total - 1;
    s.playhead = ph;

    // A synthesised deck says so, ALWAYS (D6) — it is a re-derivation, never
    // presented as the original run's captured bytes.
    if (idx.synthesized)
        s.banner = "SYNTHESIZED: this register history was RE-DERIVED by "
                   "re-running the recorded code under the emulator — sound and "
                   "exact, but not the bytes the original run observed (30 R3 T4)";

    if (idx.truncated) {
        // No data is not zero data: name the dropped prefix as a torn edge.
        std::string torn =
            "TORN: " + std::to_string(idx.dropped) +
            " step(s) dropped before the first held step — the ring "
            "evicted them; the register file at steps 0.." +
            std::to_string(idx.dropped - 1) + " is UNKNOWN, not zero";
        s.banner = s.banner.empty() ? torn : s.banner + " | " + torn;
    }

    const RegFile *rf = idx.at_step(ph);
    if (rf == nullptr) {
        // Inside the torn region: an honest blank, never a synthesised file.
        s.torn_here = true;
        s.at_held = false;
        return s;
    }

    s.at_held = true;
    s.has_prev = rf->has_prev;
    for (const RegField &f : rf->fields) {
        dt_scrubber_reg r;
        r.name = f.name;
        r.value = f.value;
        r.changed = f.changed;
        s.regs.push_back(r);
    }
    return s;
}

std::string dt_scrubber_dump(const dt_scrubber &s) {
    std::string o;
    if (!s.present) {
        o += "no-producer\n";
        o += "message=" + s.absent_message + "\n";
        if (s.synthesizable) {
            o += "synthesizable=1\n";
            o += "action=" + s.synth_action + "\n";
            o += "label=" + s.synth_label + "\n";
        }
        o += "docs=" + s.docs + "\n";
        return o;
    }
    o += "desc=" + s.desc + "\n";
    o += "steps: total=" + std::to_string(s.total) + " held=[" +
         std::to_string(s.first_held) + ".." + std::to_string(s.last_held) +
         "] dropped=" + std::to_string(s.dropped) + "\n";
    if (!s.banner.empty())
        o += "banner=" + s.banner + "\n";
    o += "playhead=" + std::to_string(s.playhead);
    if (s.torn_here) {
        o += " TORN (this step was dropped — register file UNKNOWN)\n";
        return o;
    }
    o += s.has_prev ? " (diff vs previous held step)\n"
                    : " (no previous held step — baseline absent)\n";
    for (const dt_scrubber_reg &r : s.regs)
        o +=
            "  " + r.name + "=" + hex(r.value) + (r.changed ? " *" : "") + "\n";
    return o;
}

} // namespace asmdesk
