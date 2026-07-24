// annex.cpp — the pure cross-feed join of annex.h. No ImGui, no I/O, no engine.
#include "loom/annex.h"

#include <algorithm>
#include <cstdio>

#include "asmtest_taint.h" // pure: at_taint_hit_t's field vocabulary
#include "asmtest_trace.h" // pure: asmtest_srcmap_entry_t's field vocabulary

namespace asmdesk {

namespace {

std::string hex(uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

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

const std::vector<Event> *kind_of(const Recording &r, const char *k) {
    auto it = r.by_kind.find(k);
    return it == r.by_kind.end() ? nullptr : &it->second;
}

// Did the fabric ITSELF write these bytes? That — and only that — is what
// promotes an entry from "recorded elsewhere" to "corroborates".
bool fabric_wrote(const loom_fabric_t &f, uint32_t lane, uint64_t lo,
                  uint64_t hi) {
    for (const loom_span_t &s : f.spans) {
        if (s.lane != lane || s.born_untraced)
            continue;
        if (f.lanes[lane].kind == loom_lane_kind::reg)
            return true;
        if (lo < s.hi && hi > s.lo)
            return true;
    }
    return false;
}

// Does any span on this lane carry this exact value? The ONLY thing a register
// lane and a watched address can share. It is not evidence they are the same
// place, and the entry that uses it says so in its own text.
bool fabric_holds_value(const loom_fabric_t &f, uint32_t lane, uint64_t value) {
    for (const loom_span_t &s : f.spans)
        if (s.lane == lane && s.value_valid && s.value == value)
            return true;
    return false;
}

bool fabric_ran(const loom_fabric_t &f, uint64_t off) {
    for (uint64_t o : f.insn_off)
        if (o != UINT64_MAX && o == off)
            return true;
    return false;
}

} // namespace

const char *annex_kind_name(annex_kind k) {
    switch (k) {
    case annex_kind::watch_hit:
        return "watch_hit";
    case annex_kind::taint_hit:
        return "taint_hit";
    case annex_kind::srcmap_row:
        return "srcmap_row";
    case annex_kind::ibs_edge:
        return "ibs_edge";
    }
    return "?";
}

std::string annex_verdict_text(const annex_entry_t &e) {
    if (e.verdict == annex_verdict::corroborates)
        return "corroborates";
    return "recorded by " +
           (e.producer.empty() ? std::string("(unnamed producer)")
                               : e.producer) +
           " — unconfirmed here";
}

size_t loom_annex_join(const Workspace &ws, const loom_fabric_t &f,
                       uint32_t lane, int self,
                       std::vector<annex_entry_t> *out) {
    if (out == nullptr || lane >= f.lanes.size())
        return 0;
    out->clear();
    const loom_lane_t &L = f.lanes[lane];
    const bool is_band = L.kind == loom_lane_kind::mem_band;
    uint64_t code_lo = 0, code_hi = 0;
    const bool have_code = f.code_range(&code_lo, &code_hi);

    for (size_t ri = 0; ri < ws.recordings.size(); ri++) {
        if (self >= 0 && static_cast<size_t>(self) == ri)
            continue; // a recording cannot corroborate itself
        const Recording &r = ws.recordings[ri];
        const std::string id = recording_id(r.path);
        const bool exact = !r.statistical();
        const std::string chip = id + " / " + r.provenance.backend + " / " +
                                 (exact ? "exact" : "statistical");

        auto add = [&](annex_kind k, annex_space sp, uint64_t addr, bool corr,
                       std::string text) {
            annex_entry_t e;
            e.kind = k;
            e.space = sp;
            e.exact = exact;
            e.producer = r.provenance.backend;
            e.provenance = chip;
            e.addr = addr;
            e.text = std::move(text);
            e.verdict =
                corr ? annex_verdict::corroborates : annex_verdict::unconfirmed;
            if (!exact) {
                e.lost = r.drops_lost;
                e.throttled = r.drops_throttled;
            }
            out->push_back(std::move(e));
        };

        // --- watch hits (schema `watch`) -----------------------------------
        if (const auto *ev = kind_of(r, "watch")) {
            for (const Event &x : *ev) {
                uint64_t addr = 0, pc = 0, value = 0;
                uint32_t len = 0;
                int is_write = -1;
                bool value_ok = false;
                std::string func, module;
                get(x.body, "addr", addr);
                get(x.body, "pc", pc);
                get(x.body, "is_write", is_write);
                get(x.body, "value_ok", value_ok);
                get(x.body, "value_len", len);
                get(x.body, "value", value);
                get(x.body, "func", func);
                get(x.body, "module", module);
                const char *dir = is_write == 1   ? "write"
                                  : is_write == 0 ? "read"
                                                  : "direction undecodable";
                std::string where =
                    func.empty() ? hex(pc)
                                 : func + " [" + module + "] @ " + hex(pc);
                if (is_band) {
                    if (addr < L.lo || addr >= L.hi)
                        continue;
                    add(annex_kind::watch_hit, annex_space::data, addr,
                        fabric_wrote(f, lane, addr, addr + (len ? len : 1)),
                        std::string(dir) + " of " + hex(addr) + " by " + where +
                            (value_ok ? " = " + hex(value)
                                      : " (value not read back)"));
                } else if (value_ok && fabric_holds_value(f, lane, value)) {
                    // A register lane and a watched ADDRESS are different
                    // places. The only thing they can share is a value, and
                    // "same value, different place" is informational, not
                    // evidence — the label says exactly that.
                    add(annex_kind::watch_hit, annex_space::value_only, addr,
                        false,
                        "same value, different place — informational: " +
                            hex(value) + " watched at " + hex(addr) + " by " +
                            where);
                }
            }
        }

        // --- taint hits (RESERVED kind `taint`) ----------------------------
        // The registry claims the id; the fields are NOT frozen. These are read
        // defensively against at_taint_hit_t's vocabulary, and a hit missing
        // them simply does not join.
        if (const auto *ev = kind_of(r, "taint")) {
            for (const Event &x : *ev) {
                uint64_t off = 0, ea = 0, seed_off = 0;
                uint32_t depth = 0, tag = 0;
                get(x.body, "off", off);
                get(x.body, "ea", ea);
                get(x.body, "seed_off", seed_off);
                get(x.body, "depth", depth);
                get(x.body, "tag", tag);
                std::string what = "taint tag " + hex(tag) + " seeded at " +
                                   hex(seed_off) + ", depth " +
                                   std::to_string(depth);
                if (is_band && ea >= L.lo && ea < L.hi)
                    add(annex_kind::taint_hit, annex_space::data, ea,
                        fabric_wrote(f, lane, ea, ea + 1),
                        what + ", reaching " + hex(ea));
                else if (!is_band && have_code && off >= code_lo &&
                         off <= code_hi)
                    add(annex_kind::taint_hit, annex_space::code, off,
                        fabric_ran(f, off), what + ", sink at " + hex(off));
            }
        }

        // --- source-map rows (RESERVED kind `srcmap`) ----------------------
        // CODE space only, like the taint sink offsets and the IBS edges below:
        // a line-map row is a fact about an instruction, and hanging it off a
        // memory band would assert that the line touched those bytes.
        if (const auto *ev = is_band ? nullptr : kind_of(r, "srcmap")) {
            for (const Event &x : *ev) {
                uint64_t off = 0;
                int32_t value = 0;
                uint32_t k = 0, col = 0;
                std::string file;
                get(x.body, "off", off);
                get(x.body, "value", value);
                get(x.body, "kind", k);
                get(x.body, "col", col);
                get(x.body, "file", file);
                if (!have_code || off < code_lo || off > code_hi)
                    continue;
                const char *kn = k == ASMTEST_SRC_LINE  ? "line"
                                 : k == ASMTEST_SRC_IL  ? "IL offset"
                                 : k == ASMTEST_SRC_BCI ? "bytecode index"
                                                        : "source value";
                add(annex_kind::srcmap_row, annex_space::code, off,
                    fabric_ran(f, off),
                    std::string(kn) + " " + std::to_string(value) +
                        (file.empty() ? "" : " in " + file) + " at " +
                        hex(off));
            }
        }

        // --- IBS survey edges (schema `survey`) — ANNEX ONLY ---------------
        // Never woven (loom_fabric_build refuses a statistical provenance);
        // here they corroborate a code range and carry their drop accounting.
        if (const auto *ev = kind_of(r, "survey")) {
            uint64_t samples = 0, lost = 0;
            for (const Event &x : *ev) {
                get(x.body, "samples", samples);
                get(x.body, "lost", lost);
                auto edges = x.body.find("edges");
                if (edges == x.body.end() || !edges->is_array())
                    continue;
                for (const auto &y : *edges) {
                    if (is_band)
                        continue; // a branch edge is a CODE fact, never a data one
                    uint64_t from = 0, to = 0, count = 0;
                    get(y, "from_addr", from);
                    get(y, "to_addr", to);
                    get(y, "count", count);
                    if (!have_code)
                        continue;
                    bool in = (from >= code_lo && from <= code_hi) ||
                              (to >= code_lo && to <= code_hi);
                    if (!in)
                        continue;
                    annex_entry_t e;
                    e.kind = annex_kind::ibs_edge;
                    e.space = annex_space::code;
                    e.exact = false;
                    e.producer = r.provenance.backend;
                    e.provenance = chip;
                    e.addr = from;
                    e.lost = lost;
                    e.throttled = r.drops_throttled;
                    // A sampled edge NEVER corroborates: a histogram bucket is
                    // not an observation of a particular execution.
                    e.verdict = annex_verdict::unconfirmed;
                    e.text =
                        hex(from) + " -> " + hex(to) + ", " +
                        std::to_string(count) + " of " +
                        std::to_string(samples) + " samples" +
                        (lost ? "; " + std::to_string(lost) + " lost" : "");
                    out->push_back(std::move(e));
                }
            }
        }
    }
    return out->size();
}

std::string loom_annex_dump(const std::vector<annex_entry_t> &rows) {
    std::string o;
    for (const annex_entry_t &e : rows) {
        o += annex_kind_name(e.kind);
        o += e.space == annex_space::data   ? " space=data"
             : e.space == annex_space::code ? " space=code"
                                            : " space=value_only";
        o += " " + annex_verdict_text(e);
        o += " [" + e.provenance + "]";
        if (!e.exact && (e.lost || e.throttled))
            o += " drops{lost=" + std::to_string(e.lost) +
                 (e.throttled ? ",throttled" : "") + "}";
        o += " " + e.text + "\n";
    }
    return o;
}

} // namespace asmdesk
