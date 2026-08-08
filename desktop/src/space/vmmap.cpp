// vmmap.cpp — see vmmap.h for the one rule this must not break (a vmmap span is
// never a Projection region).
#include "space/vmmap.h"

#include <algorithm>

#include "space/projection.h" // kObservedDataLabel

namespace asmdesk::space {

namespace {

// The wire carries addresses as hex STRINGS, because a JSON number is a double
// in many readers and silently rounds a 64-bit pointer. There is no shared hex
// parser reachable from space/ (terrain.cpp, pick.cpp and standalone.cpp each
// carry their own private hex FORMATTER for the same reason), so this TU carries
// its own reader. Tolerates an absent "0x"; stops at the first non-hex byte
// rather than throwing, since a malformed field must degrade to "unusable span"
// and be dropped, not abort the decode of the whole recording.
uint64_t parse_hex(const std::string &s) {
    uint64_t v = 0;
    size_t i =
        (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 2 : 0;
    for (; i < s.size(); i++) {
        const char c = s[i];
        uint64_t d;
        if (c >= '0' && c <= '9')
            d = static_cast<uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'f')
            d = static_cast<uint64_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            d = static_cast<uint64_t>(c - 'A' + 10);
        else
            break;
        v = (v << 4) | d;
    }
    return v;
}

} // namespace

Region::Kind vmmap_kind_of(const std::string &perms, const std::string &name) {
    if (name == "[heap]")
        return Region::Heap;
    if (name.rfind("[stack", 0) == 0) // "[stack]" and "[stack:7]"
        return Region::Stack;
    // Any other bracket token ([vdso], [vvar], [vsyscall]) is a kernel
    // pseudo-mapping, not a module — Mmap whatever its permissions say.
    if (name.empty() || name[0] == '[')
        return Region::Mmap;
    const bool exec = perms.size() > 2 && perms[2] == 'x';
    return exec ? Region::Code : Region::Data;
}

VmMap vmmap_from_recording(const Recording &rec) {
    VmMap m;
    auto it = rec.by_kind.find("vmmap");
    if (it == rec.by_kind.end() || it->second.empty())
        return m; // `present` stays false: no event, not an empty measurement
    const Event &e = it->second.back(); // last wins; see the header
    m.present = true;
    // An older producer that omitted the flag expressed no opinion; stamping
    // "unreadable" on it would manufacture the opposite error, so default true —
    // the same rule procinfo's code.maps_readable documents for itself.
    m.readable = e.body.value("maps_readable", true);
    m.spans_total = e.body.value("spans_total", uint64_t{0});
    m.truncated = e.body.value("spans_truncated", false);
    if (!m.readable)
        return m; // absent measurement: classify nothing
    auto sp = e.body.find("spans");
    if (sp == e.body.end() || !sp->is_array())
        return m;
    for (const auto &s : *sp) {
        if (!s.is_object())
            continue;
        VmSpan v;
        v.base = parse_hex(s.value("base", std::string()));
        v.len = s.value("len", uint64_t{0});
        v.perms = s.value("perms", std::string());
        v.name = s.value("name", std::string());
        v.path = s.value("path", std::string());
        if (v.len > 0) // a zero-length row cannot contain anything
            m.spans.push_back(std::move(v));
    }
    std::sort(m.spans.begin(), m.spans.end(),
              [](const VmSpan &a, const VmSpan &b) { return a.base < b.base; });
    // A producer that capped its rows states the pre-cap total; one that did not
    // may omit it. Never report fewer than we hold.
    if (m.spans_total < m.spans.size())
        m.spans_total = m.spans.size();
    return m;
}

size_t vmmap_apply_names(std::vector<Region> &regions, const VmMap &map) {
    if (!map.present || !map.readable || map.spans.empty())
        return 0;
    size_t n = 0;
    for (Region &r : regions) {
        // Only an UNNAMED observed-data span. A codeimage region already states
        // its own captured provenance (see the header).
        if (r.kind != Region::Unknown || r.label != kObservedDataLabel)
            continue;
        // The last span starting at or before r.base is the only one that can
        // contain it, since spans are sorted and the kernel's maps do not
        // overlap. Half-open [base, base+len): a region starting exactly at a
        // span's END belongs to the next span or to nothing, never to this one —
        // otherwise every gap silently inherits its predecessor's name.
        auto hi = std::upper_bound(
            map.spans.begin(), map.spans.end(), r.base,
            [](uint64_t a, const VmSpan &s) { return a < s.base; });
        if (hi == map.spans.begin())
            continue;
        const VmSpan &s = *(hi - 1);
        if (r.base < s.base || r.base >= s.base + s.len)
            continue; // covered by no mapping: still unknown, never guessed
        r.kind = vmmap_kind_of(s.perms, s.name);
        r.label = s.name.empty() ? std::string("(anonymous)") : s.name;
        r.extent_base = s.base;
        r.extent_len = s.len;
        r.perms = s.perms;
        r.path = s.path;
        n++;
    }
    return n;
}

} // namespace asmdesk::space
