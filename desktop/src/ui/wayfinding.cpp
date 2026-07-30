// wayfinding.cpp — the persistent context chrome (see wayfinding.h).
#include "ui/wayfinding.h"

#include <cstdio>
#include <functional>
#include <vector>

#include "imgui.h"

#include "nav.h"
#include "ui/shell.h"

namespace asmdesk {

namespace {

std::string base_name(const std::string &path) {
    size_t slash = path.find_last_of('/');
    std::string b = slash == std::string::npos ? path : path.substr(slash + 1);
    return b.empty() ? "(unnamed)" : b;
}

// The parent-dir components of a path, in order, excluding the basename and any
// empty segments: "/a/b/c/x.asmtrace" -> {"a","b","c"}.
std::vector<std::string> parent_components(const std::string &path) {
    std::vector<std::string> comps;
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos)
        return comps; // no parent dir at all
    std::string dir = path.substr(0, slash);
    size_t i = 0;
    while (i < dir.size()) {
        size_t j = dir.find('/', i);
        if (j == std::string::npos)
            j = dir.size();
        if (j > i)
            comps.push_back(dir.substr(i, j - i));
        i = j + 1;
    }
    return comps;
}

// Join the LAST k components with '/': tail({"a","b","c"}, 2) -> "b/c".
std::string tail_join(const std::vector<std::string> &comps, size_t k) {
    if (k > comps.size())
        k = comps.size();
    std::string out;
    for (size_t i = comps.size() - k; i < comps.size(); i++) {
        if (!out.empty())
            out += '/';
        out += comps[i];
    }
    return out;
}

std::string short_hash(const std::string &s) {
    size_t h = std::hash<std::string>{}(s);
    char buf[8];
    std::snprintf(buf, sizeof buf, "%06x", static_cast<unsigned>(h & 0xFFFFFF));
    return buf;
}

} // namespace

std::string disambiguated_label(const Workspace &ws, size_t i) {
    if (i >= ws.recordings.size())
        return "(none)";
    const std::string &path = ws.recordings[i].path;
    std::string base = base_name(path);

    // Which OTHER open recordings share this basename?
    std::vector<size_t> peers;
    for (size_t j = 0; j < ws.recordings.size(); j++)
        if (j != i && base_name(ws.recordings[j].path) == base)
            peers.push_back(j);
    if (peers.empty())
        return base; // already unique — stable label, no suffix

    // Smallest k such that the last-k parent-dir components tell `i` apart from
    // every same-basename peer.
    std::vector<std::string> mine = parent_components(path);
    for (size_t k = 1; k <= mine.size(); k++) {
        std::string tail = tail_join(mine, k);
        bool unique = true;
        for (size_t j : peers)
            if (tail_join(parent_components(ws.recordings[j].path), k) == tail) {
                unique = false;
                break;
            }
        if (unique)
            return base + " — " + tail;
    }
    // Even the full parent path collides (the same file opened twice, or two bare
    // same-name files): a short hash SEEDED WITH THE WORKSPACE INDEX is the
    // last-resort distinguisher — a hash of the path alone would be identical for
    // identical paths, so the index is what keeps the two labels apart.
    return base + " — #" + short_hash(path + "\x1f" + std::to_string(i));
}

// The index of the open recording whose deep-link id is `rec` (its basename), or
// -1. Used to disambiguate the breadcrumb's recording segment.
static int index_of_rec(const ShellState &s, const std::string &rec) {
    for (size_t i = 0; i < s.streams.size(); i++)
        if (s.streams[i].id == rec)
            return static_cast<int>(i);
    return -1;
}

BreadcrumbModel breadcrumb_model(const ShellState &s) {
    BreadcrumbModel m;
    if (!s.nav.current) {
        m.has_position = false;
        m.prompt = "no position — open a recording or press Ctrl+Shift+P";
        return m;
    }
    const dt_link &l = *s.nav.current;
    m.has_position = true;

    int ri = index_of_rec(s, l.rec);
    m.recording = ri >= 0 ? disambiguated_label(s.ws, static_cast<size_t>(ri))
                          : l.rec;
    if (!l.rec_b.empty()) {
        int bi = index_of_rec(s, l.rec_b);
        m.diff_b = "vs " + (bi >= 0 ? disambiguated_label(s.ws,
                                                          static_cast<size_t>(bi))
                                    : l.rec_b);
    }
    // The live coordinate: a link carries a process (pid), not a thread, in v1 —
    // so the session/scope segment is the process it drilled into, faithfully named
    // rather than a fabricated thread id.
    if (l.pid) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "pid %ld", *l.pid);
        m.session = buf;
        m.thread = buf;
    }
    m.view = dt_view_name(l.view);
    if (l.step) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "step %u", *l.step);
        m.selection = buf;
    } else if (l.off) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "off 0x%llx",
                      static_cast<unsigned long long>(*l.off));
        m.selection = buf;
    }
    // The active recording's client-side filter, if any. Stating scope, not
    // implying the hidden rows are gone (D7, the "showing N of M" ethos).
    if (s.active_tab >= 0 &&
        static_cast<size_t>(s.active_tab) < s.observers.size()) {
        const char *f = s.observers[static_cast<size_t>(s.active_tab)]
                            .syscall_filter;
        if (f && f[0] != '\0')
            m.filter = f;
    }
    return m;
}

void draw_wayfinding_bar(ShellState &s) {
    BreadcrumbModel m = breadcrumb_model(s);
    if (!m.has_position) {
        ImGui::TextDisabled("%s", m.prompt.c_str());
        return;
    }
    // recording ▸ [vs B] ▸ [session] ▸ view   · selection.  The separator glyph
    // is U+25B8; the model fields carry the real words the tests assert.
    const char *sep = "  \xE2\x96\xB8  ";
    std::string line = m.recording;
    if (!m.diff_b.empty())
        line += sep + m.diff_b;
    if (!m.session.empty())
        line += sep + m.session;
    line += sep + m.view;
    if (!m.selection.empty())
        line += "   \xC2\xB7 " + m.selection;
    ImGui::TextUnformatted(line.c_str());
    if (!m.filter.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled(" \xC2\xB7 filter: %s (scope, not absence)",
                            m.filter.c_str());
    }
}

} // namespace asmdesk
