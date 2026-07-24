// syscalls.cpp — the pure builder + dump of syscalls.h. No ImGui, no I/O.
#include "views/syscalls.h"

namespace asmdesk {

SyscallView obs_syscalls_build(const Recording &r, const ObsLifecycle *lc) {
    SyscallView v;
    const ObsLifecycle life = lc ? *lc : obs_lifecycle_of(r);
    v.chrome = obs_chrome(r);
    v.record_redacted = r.provenance.redacted;

    auto it = r.by_kind.find("syscall");
    if (it != r.by_kind.end()) {
        for (const Event &e : it->second) {
            SyscallRow row;
            row.index = v.rows.size();
            row.line = e.body.value("line", std::string());
            if (e.body.contains("payload") && e.body["payload"].is_string()) {
                row.has_payload = true;
                row.payload = e.body["payload"].get<std::string>();
            }
            if (e.body.contains("tid") && e.body["tid"].is_number_integer())
                row.tid = e.body["tid"].get<long>();
            v.rows.push_back(std::move(row));
        }
    }
    // A session's `started` params echo says what the capture was actually
    // running with, which is the only honest source for the follow flag: what
    // the client asked for and what it got are not the same field.
    const nlohmann::json *p = obs_started_params(life, "log");
    if (p != nullptr && p->contains("follow") && (*p)["follow"].is_boolean())
        v.follow = (*p)["follow"].get<bool>();
    v.revealed.assign(v.rows.size(), 0);
    return v;
}

std::string obs_syscall_payload_cell(const SyscallView &v, size_t i) {
    if (i >= v.rows.size())
        return "";
    const SyscallRow &row = v.rows[i];
    if (!row.has_payload) {
        // Two different absences, and they must not read alike: the producer
        // withheld it, or this syscall carried no content in the first place.
        return v.record_redacted ? "(redacted at record time — not in this "
                                   "recording)"
                                 : "(no payload)";
    }
    const bool shown =
        v.reveal_all || (i < v.revealed.size() && v.revealed[i] != 0);
    if (shown)
        return row.payload;
    // The byte count is structure, not content — the payload-free `line`
    // already carries it as `<N bytes>` — so showing it here costs nothing and
    // tells the reader whether revealing is worth a click.
    return "••• hidden (" + std::to_string(row.payload.size()) + " bytes)";
}

void obs_syscall_reveal(SyscallView &v, size_t i, bool on) {
    if (i >= v.revealed.size())
        return;
    v.revealed[i] = on ? 1 : 0;
    v.reveal_all_armed = false;
}

bool obs_syscall_reveal_all(SyscallView &v) {
    if (!v.reveal_all_armed) {
        v.reveal_all_armed = true; // arm only; the caller draws the prompt
        return false;
    }
    v.reveal_all_armed = false;
    v.reveal_all = true;
    return true;
}

std::string obs_syscall_reveal_all_prompt(const SyscallView &v) {
    size_t n = 0;
    for (const SyscallRow &r : v.rows)
        if (r.has_payload)
            n++;
    return "reveal the content of " + std::to_string(n) +
           " syscall(s) — every path, buffer and sockaddr this process passed "
           "through the kernel, on screen at once. Nothing is written back to "
           "the recording.";
}

const char *obs_syscall_tid_note() {
    return "no tid filter: the syscalls engine takes no only_tid parameter, so "
           "every thread is followed and the engine tags each line with its "
           "own "
           "\"[tid] \" prefix — a filter here would hide rows, not narrow the "
           "capture";
}

std::string obs_syscalls_dump(const SyscallView &v) {
    std::string s;
    if (!v.chrome.banner.empty())
        s += "banner=" + v.chrome.banner + "\n";
    s += "chrome=" + obs_chrome_chip(v.chrome) + "\n";
    s += "record_redacted=" + std::string(v.record_redacted ? "yes" : "no") +
         " reveal_all=" + (v.reveal_all ? "yes" : "no") +
         " follow=" + (v.follow ? "yes" : "no") + "\n";
    s += "rows=" + std::to_string(v.rows.size()) + "\n";
    for (size_t i = 0; i < v.rows.size(); i++) {
        const SyscallRow &r = v.rows[i];
        s += "  " + std::to_string(i) + " ";
        if (r.tid >= 0)
            s += "[tid " + std::to_string(r.tid) + "] ";
        s += r.line + " | payload=" + obs_syscall_payload_cell(v, i) + "\n";
    }
    return s;
}

} // namespace asmdesk
