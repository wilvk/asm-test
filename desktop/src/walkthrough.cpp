// walkthrough.cpp — the pure decode of walkthrough.h. No ImGui, no I/O.
#include "walkthrough.h"

#include <algorithm>

#include "doc/streams.h"

namespace asmdesk {

const char *const kWtBeyondWindow =
    "stop is beyond the recorded window — this recording is truncated, so the "
    "step this stop is about was never recorded";

namespace {

template <typename T> void get(const nlohmann::json &j, const char *k, T &out) {
    auto it = j.find(k);
    if (it == j.end())
        return;
    if constexpr (std::is_same_v<T, std::string>) {
        if (it->is_string())
            out = it->get<std::string>();
    } else if constexpr (std::is_same_v<T, bool>) {
        if (it->is_boolean())
            out = it->get<bool>();
    } else {
        if (it->is_number_integer() || it->is_number_unsigned())
            out = it->get<T>();
    }
}

} // namespace

bool wt_model::anchor_in_window(long step) const {
    if (step < 0)
        return true; // unanchored stops are always playable
    return static_cast<uint32_t>(step) < steps_recorded;
}

const wt_stop *wt_model::current() const {
    if (cur < 0 || cur >= static_cast<int>(stops.size()))
        return nullptr;
    return &stops[static_cast<size_t>(cur)];
}

bool wt_model::next() {
    if (cur + 1 >= static_cast<int>(stops.size()))
        return false;
    cur++;
    return true;
}

bool wt_model::prev() {
    if (cur <= 0)
        return false;
    cur--;
    return true;
}

wt_model wt_build(const Recording &r) {
    wt_model m;
    m.id = recording_id(r.path);
    m.truncated = r.truncated();
    m.torn = r.torn;

    // How many steps this recording actually holds. `trace` events are the
    // ordered instruction stream; a stop's `step` indexes into it.
    auto tr = r.by_kind.find("trace");
    if (tr != r.by_kind.end())
        m.steps_recorded = static_cast<uint32_t>(tr->second.size());

    auto notes = r.by_kind.find("note");
    if (notes == r.by_kind.end())
        return m;

    int ordinal = 0;
    for (const Event &e : notes->second) {
        bool stop = false;
        get(e.body, "stop", stop);
        std::string text;
        get(e.body, "text", text);
        if (!stop) {
            // The first un-stopped note is the walkthrough's own title line.
            if (m.title.empty())
                m.title = text;
            continue;
        }
        wt_stop s;
        s.ordinal =
            ++ordinal; // file order IS ordinal order (the schema's rule)
        s.body = text;
        long step = -1;
        get(e.body, "step", step);
        s.step_anchor = step;
        get(e.body, "title", s.title);
        get(e.body, "expected", s.expected);
        get(e.body, "got", s.got);
        m.stops.push_back(std::move(s));
    }
    return m;
}

std::string wt_dump(const wt_model &m) {
    std::string o = "walkthrough " + m.id +
                    " steps_recorded=" + std::to_string(m.steps_recorded) +
                    (m.truncated ? " TRUNCATED" : "") +
                    (m.torn ? " TORN" : "") + "\n";
    if (!m.title.empty())
        o += "title " + m.title + "\n";
    for (const wt_stop &s : m.stops) {
        o += "stop " + std::to_string(s.ordinal);
        o += s.step_anchor < 0 ? " unanchored"
                               : " step=" + std::to_string(s.step_anchor);
        if (!m.anchor_in_window(s.step_anchor))
            o += " BEYOND_WINDOW";
        if (!s.title.empty())
            o += " title=\"" + s.title + "\"";
        if (s.has_framing())
            o += " expected=\"" + s.expected + "\" got=\"" + s.got + "\"";
        o += "\n";
    }
    return o;
}

} // namespace asmdesk
