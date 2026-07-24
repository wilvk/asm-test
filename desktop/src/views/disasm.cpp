// disasm.cpp — the pure builder + byte resolution of disasm.h. No ImGui, no
// I/O, and — the point of the whole file — no live memory read.
#include "views/disasm.h"

#include <algorithm>
#include <cstdio>
#include <set>

namespace asmdesk {

namespace {

std::string hex(uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

int nibble(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// Decode the schema's lowercase hex. A malformed or odd-length string yields
// what parsed and stops: the bytes we have are real, the rest were never there.
std::vector<uint8_t> unhex(const std::string &s) {
    std::vector<uint8_t> out;
    out.reserve(s.size() / 2);
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        int hi = nibble(s[i]), lo = nibble(s[i + 1]);
        if (hi < 0 || lo < 0)
            break;
        out.push_back(static_cast<uint8_t>(hi * 16 + lo));
    }
    return out;
}

std::string to_hex(const uint8_t *p, size_t n) {
    static const char *d = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) {
        s += d[p[i] >> 4];
        s += d[p[i] & 0xF];
    }
    return s;
}

} // namespace

std::vector<uint64_t> DisasmView::spans() const {
    std::set<uint64_t> bases;
    for (const CodeVersion &c : versions)
        bases.insert(c.base);
    return std::vector<uint64_t>(bases.begin(), bases.end());
}

DisasmView obs_disasm_build(const Recording &r, const ObsLifecycle *lc) {
    DisasmView v;
    (void)lc; // no lifecycle fact feeds this pane yet; the signature is uniform
              // so a caller never has to remember which views take one
    v.chrome = obs_chrome(r);

    auto ci = r.by_kind.find("codeimage");
    if (ci != r.by_kind.end()) {
        for (const Event &e : ci->second) {
            CodeVersion c;
            c.base = e.body.value("base", uint64_t{0});
            c.len = e.body.value("len", uint64_t{0});
            c.version = e.body.value("version", uint64_t{0});
            c.when = e.body.value("when", uint64_t{0});
            c.bytes = unhex(e.body.value("bytes", std::string()));
            // A span whose hex is short is not a span of zeros: clamp `len` to
            // what actually arrived so a lookup past it reports UNKNOWN rather
            // than handing back bytes nobody recorded.
            if (c.bytes.size() < c.len)
                c.len = c.bytes.size();
            v.versions.push_back(std::move(c));
        }
    }

    auto tr = r.by_kind.find("trace");
    if (tr != r.by_kind.end()) {
        for (const Event &e : tr->second) {
            if (v.trace_basis.empty())
                v.trace_basis = e.body.value("basis", std::string());
            if (e.body.contains("disasm") && e.body["disasm"].is_string())
                v.recorded_disasm[e.body.value("off", uint64_t{0})] =
                    e.body["disasm"].get<std::string>();
        }
    }

    // The producer's measured reason for having no code image at all. Matched
    // by prefix so the note stays human-readable text rather than a code.
    auto nt = r.by_kind.find("note");
    if (nt != r.by_kind.end()) {
        for (const Event &e : nt->second) {
            std::string t = e.body.value("text", std::string());
            if (t.rfind("codeimage unavailable: ", 0) == 0)
                v.unavailable_reason =
                    t.substr(std::string("codeimage unavailable: ").size());
        }
    }
    return v;
}

CodeBytes obs_disasm_bytes_at(const DisasmView &v, uint64_t addr,
                              uint64_t when) {
    CodeBytes out;
    const CodeVersion *best = nullptr;
    bool covered = false;
    for (const CodeVersion &c : v.versions) {
        if (!c.covers(addr))
            continue;
        covered = true;
        // `when == 0` means the latest, exactly as asmtest_codeimage_bytes_at
        // defines it; otherwise the greatest `when` at or before the query.
        if (when != 0 && c.when > when)
            continue;
        if (best == nullptr || c.when > best->when ||
            (c.when == best->when && c.version > best->version))
            best = &c;
    }
    if (best == nullptr) {
        out.why = covered
                      ? "no code image of " + hex(addr) +
                            " at or before time " + std::to_string(when) +
                            " — the bytes there are UNKNOWN, not the ones from "
                            "a later version (a JIT reuses addresses)"
                      : hex(addr) + " is in no tracked region — nothing "
                                    "captured its bytes";
        return out;
    }
    size_t off = static_cast<size_t>(addr - best->base);
    out.found = true;
    out.version = best;
    out.data = best->bytes.data() + off;
    out.len = best->bytes.size() - off;
    return out;
}

std::vector<DisasmRow> obs_disasm_rows(const DisasmView &v,
                                       const std::vector<uint64_t> &addrs,
                                       uint64_t when, size_t bytes_per_row) {
    std::vector<DisasmRow> rows;
    for (uint64_t a : addrs) {
        DisasmRow row;
        row.addr = a;
        CodeBytes b = obs_disasm_bytes_at(v, a, when);
        if (b.found) {
            size_t n = std::min(b.len, bytes_per_row);
            row.bytes = to_hex(b.data, n);
            row.source = "codeimage v" + std::to_string(b.version->version);
        }
        auto d = v.recorded_disasm.find(a);
        if (d != v.recorded_disasm.end()) {
            row.text = d->second;
            if (!b.found)
                // D10's fallback, and it is LABELLED: recorded text is what the
                // producer saw at record time, which is a weaker claim than the
                // bytes and must not read like them.
                row.source = "recorded disasm";
        }
        if (row.source.empty())
            row.source = "unknown";
        rows.push_back(std::move(row));
    }
    return rows;
}

std::string obs_disasm_dump(const DisasmView &v) {
    std::string s;
    if (!v.chrome.banner.empty())
        s += "banner=" + v.chrome.banner + "\n";
    if (!v.unavailable_reason.empty())
        s += "no code image: " + v.unavailable_reason + "\n";
    s += "versions=" + std::to_string(v.versions.size()) +
         " spans=" + std::to_string(v.spans().size()) +
         " recorded_disasm=" + std::to_string(v.recorded_disasm.size()) + "\n";
    for (const CodeVersion &c : v.versions)
        s += "  " + hex(c.base) + "+" + std::to_string(c.len) + " v" +
             std::to_string(c.version) + " when=" + std::to_string(c.when) +
             " " + to_hex(c.bytes.data(), c.bytes.size()) + "\n";
    return s;
}

} // namespace asmdesk
