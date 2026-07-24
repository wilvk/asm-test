// nav.cpp — parse/format/route for the deep-link spine (nav.h).
#include "nav.h"

#include <cstdio>
#include <cstdlib>

namespace asmdesk {

namespace {

constexpr char kScheme[] = "asmtrace-link:";

// Percent-encode everything outside the unreserved set. Recording ids are
// filenames and can carry `&`, `=`, spaces — a link that did not escape them
// would parse back as a different link, which is the one thing a round-trip
// contract cannot allow.
std::string enc(const std::string &s) {
    static const char *hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                     c == '.' || c == '~' || c == '/';
        if (plain) {
            o += static_cast<char>(c);
        } else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 0xF];
        }
    }
    return o;
}

int hexval(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// Decode; false on a malformed escape (loud, not silently dropped).
bool dec(std::string_view s, std::string &out) {
    out.clear();
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '%') {
            out += s[i];
            continue;
        }
        if (i + 2 >= s.size())
            return false;
        int hi = hexval(s[i + 1]), lo = hexval(s[i + 2]);
        if (hi < 0 || lo < 0)
            return false;
        out += static_cast<char>(hi * 16 + lo);
        i += 2;
    }
    return true;
}

// Parse an unsigned integer, decimal or 0x-hex. False on anything else — a
// value we cannot read is an error, never a silent 0.
bool parse_u64(const std::string &s, uint64_t &out) {
    if (s.empty())
        return false;
    const char *p = s.c_str();
    int base = 10;
    if (s.size() > 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    }
    char *end = nullptr;
    unsigned long long v = std::strtoull(p, &end, base);
    if (end == p || *end != '\0')
        return false;
    out = v;
    return true;
}

} // namespace

namespace {
// ONE table for the name mapping, both directions, so a view added to the enum
// cannot end up formattable but not parseable (or the reverse).
struct view_name {
    dt_view view;
    const char *name;
};
const view_name kViewNames[] = {
    {dt_view::canvas, "canvas"},     {dt_view::timeline, "timeline"},
    {dt_view::slice, "slice"},       {dt_view::diff, "diff"},
    {dt_view::syscalls, "syscalls"}, {dt_view::watch, "watch"},
    {dt_view::topo, "topo"},         {dt_view::hotedges, "hotedges"},
    {dt_view::tree, "tree"},         {dt_view::region, "region"},
    {dt_view::disasm, "disasm"},
};
} // namespace

const char *dt_view_name(dt_view v) {
    for (const view_name &n : kViewNames)
        if (n.view == v)
            return n.name;
    return "canvas";
}

bool dt_view_parse(std::string_view s, dt_view &out) {
    for (const view_name &n : kViewNames) {
        if (s == n.name) {
            out = n.view;
            return true;
        }
    }
    return false;
}

const std::vector<dt_view> &dt_all_views() {
    static const std::vector<dt_view> all = [] {
        std::vector<dt_view> v;
        for (const view_name &n : kViewNames)
            v.push_back(n.view);
        return v;
    }();
    return all;
}

bool dt_nav_parse(std::string_view s, dt_link &out, std::string &err) {
    err.clear();
    const std::string_view scheme(kScheme);
    if (s.substr(0, scheme.size()) != scheme) {
        err = "not an asmtrace link: it must start with \"" +
              std::string(kScheme) + "\"";
        return false;
    }
    s.remove_prefix(scheme.size());

    dt_link link;
    bool have_view = false, have_rec = false;
    while (!s.empty()) {
        size_t amp = s.find('&');
        std::string_view pair = s.substr(0, amp);
        s = amp == std::string_view::npos ? std::string_view()
                                          : s.substr(amp + 1);
        if (pair.empty())
            continue;
        size_t eq = pair.find('=');
        if (eq == std::string_view::npos) {
            err = "link field \"" + std::string(pair) + "\" has no value";
            return false;
        }
        std::string key(pair.substr(0, eq));
        std::string value;
        if (!dec(pair.substr(eq + 1), value)) {
            err = "link field \"" + key + "\" has a malformed % escape";
            return false;
        }
        if (key == "v") {
            if (!dt_view_parse(value, link.view)) {
                err = "unknown view \"" + value + "\" (expected one of: ";
                for (size_t i = 0; i < dt_all_views().size(); i++)
                    err += std::string(i ? ", " : "") +
                           dt_view_name(dt_all_views()[i]);
                err += ")";
                return false;
            }
            have_view = true;
        } else if (key == "rec") {
            link.rec = value;
            have_rec = !value.empty();
        } else if (key == "rec_b") {
            link.rec_b = value;
        } else if (key == "step") {
            uint64_t v = 0;
            if (!parse_u64(value, v) || v > 0xFFFFFFFFull) {
                err = "step \"" + value + "\" is not a 32-bit step index";
                return false;
            }
            link.step = static_cast<uint32_t>(v);
        } else if (key == "off") {
            uint64_t v = 0;
            if (!parse_u64(value, v)) {
                err = "off \"" + value + "\" is not an offset";
                return false;
            }
            link.off = v;
        } else if (key == "pid") {
            uint64_t v = 0;
            if (!parse_u64(value, v) || v == 0 || v > 0x7FFFFFFFull) {
                // 0 is not a pid, and a link that quietly meant "no process"
                // would drill into whatever happened to be selected.
                err = "pid \"" + value + "\" is not a process id";
                return false;
            }
            link.pid = static_cast<long>(v);
        }
        // else: an unknown key, IGNORED — a link from a newer build still
        // navigates here, to the extent this build understands it.
    }
    if (!have_view) {
        err = "link has no view (v=canvas|timeline|slice|diff)";
        return false;
    }
    if (!have_rec) {
        err = "link names no recording (rec=<id>)";
        return false;
    }
    out = link;
    return true;
}

std::string dt_nav_format(const dt_link &link) {
    std::string s = kScheme;
    s += "v=";
    s += dt_view_name(link.view);
    s += "&rec=" + enc(link.rec);
    if (!link.rec_b.empty())
        s += "&rec_b=" + enc(link.rec_b);
    if (link.step) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%u", *link.step);
        s += "&step=";
        s += buf;
    }
    if (link.off) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "0x%llx",
                      static_cast<unsigned long long>(*link.off));
        s += "&off=";
        s += buf;
    }
    if (link.pid) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%ld", *link.pid);
        s += "&pid=";
        s += buf;
    }
    return s;
}

void dt_nav_register(dt_nav_table &t, dt_view v, dt_nav_handler h) {
    for (auto &e : t.entries) {
        if (e.view == v) {
            e.handler = std::move(h);
            return;
        }
    }
    t.entries.push_back({v, std::move(h)});
}

bool dt_nav_go(dt_nav_table &t, const dt_link &link) {
    t.last_error.clear();
    if (t.have_recording && !t.have_recording(link.rec)) {
        t.last_error = "no open recording named \"" + link.rec +
                       "\" — open it first (the link is otherwise valid)";
        return false;
    }
    if (!link.rec_b.empty() && t.have_recording &&
        !t.have_recording(link.rec_b)) {
        t.last_error = "no open recording named \"" + link.rec_b +
                       "\" for the B side of this diff";
        return false;
    }
    for (auto &e : t.entries) {
        if (e.view == link.view) {
            if (!e.handler) {
                t.last_error = std::string("the ") + dt_view_name(link.view) +
                               " view has no handler registered";
                return false;
            }
            e.handler(link);
            t.current = link;
            return true;
        }
    }
    t.last_error = std::string("no ") + dt_view_name(link.view) +
                   " view is registered in this build";
    return false;
}

const std::vector<dt_binding> &dt_nav_bindings() {
    static const std::vector<dt_binding> b = {
        {"1 / 2 / 3 / 4", "canvas / timeline / slice explorer / diff"},
        {"j / k, Down / Up", "next / previous row or step"},
        {"PgDn / PgUp", "page down / up"},
        {"Ctrl+G", "go to a step or offset"},
        {"Enter", "open the slice explorer at the selected step"},
        {"b / f", "light the backward / forward cone from the selection"},
        {"c", "clear the cones"},
        {"[ / ]", "walk one dependence generation back / forward"},
        {"d", "attach or detach a second recording (diff)"},
        {"x", "swap A and B"},
        {"n / p", "next / previous divergence"},
        {"y", "copy a deep link to this position"},
    };
    return b;
}

} // namespace asmdesk
