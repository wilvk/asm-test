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

dt_scrubber dt_scrubber_build(const StepIndex &idx, uint64_t playhead) {
    dt_scrubber s;
    s.desc = idx.desc;

    if (!idx.present()) {
        // No per-step register producer ran. State it plainly and point at the
        // docs; do NOT offer the re-run-with-larger-max_insns fallback — it is
        // documented as NOT day-one (09-teaching-producers.md T3).
        s.present = false;
        s.absent_message =
            "no per-step register capture in this recording — the regstate "
            "producer was not run (record with asmtrace_record --steps=<cap>). "
            "Re-running with a larger max_insns to synthesise one is not a "
            "day-one feature.";
        s.docs = "docs/internal/gui/09-teaching-producers.md";
        return s;
    }

    s.present = true;
    s.dropped = idx.dropped;
    s.first_held = idx.first_step;
    s.held = idx.count();
    s.last_held = idx.first_step + idx.count() - 1;
    s.total = idx.total_steps();

    uint64_t ph = playhead;
    if (ph >= s.total)
        ph = s.total - 1;
    s.playhead = ph;

    if (idx.truncated) {
        // No data is not zero data: name the dropped prefix as a torn edge.
        s.banner = "TORN: " + std::to_string(idx.dropped) +
                   " step(s) dropped before the first held step — the ring "
                   "evicted them; the register file at steps 0.." +
                   std::to_string(idx.dropped - 1) + " is UNKNOWN, not zero";
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
