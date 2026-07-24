// watch.cpp — the pure builder + rules of watch.h. No ImGui, no I/O.
#include "views/watch.h"

#include <cstdio>

namespace asmdesk {

namespace {
std::string hex(uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}
} // namespace

WatchView obs_watch_build(const Recording &r, const ObsLifecycle *lc) {
    WatchView v;
    const ObsLifecycle life = lc ? *lc : obs_lifecycle_of(r);
    v.chrome = obs_chrome(r);
    v.skip = obs_skip(life, "watch");

    auto it = r.by_kind.find("watch");
    if (it != r.by_kind.end()) {
        for (const Event &e : it->second) {
            WatchHit h;
            h.hit_no = e.body.value("hit_no", uint64_t{0});
            h.tid = e.body.value("tid", 0L);
            h.pc = e.body.value("pc", uint64_t{0});
            h.addr = e.body.value("addr", uint64_t{0});
            // -1 is a legitimate value AND the honest default: a `watch` event
            // that carried no direction was not a write.
            h.is_write = e.body.value("is_write", -1);
            h.value_ok = e.body.value("value_ok", false);
            h.value_len = e.body.value("value_len", uint32_t{0});
            h.value = e.body.value("value", uint64_t{0});
            if (e.body.contains("func") && e.body["func"].is_string()) {
                h.func = e.body["func"].get<std::string>();
                h.have_loc = true;
            }
            h.module = e.body.value("module", std::string());
            h.off = e.body.value("off", uint64_t{0});
            v.hits.push_back(std::move(h));
        }
    }

    const nlohmann::json *p = obs_started_params(life, "watch");
    if (p != nullptr) {
        v.have_effective = true;
        v.effective.addr = p->value("addr", uint64_t{0});
        v.effective.len = p->value("len", 4);
        v.effective.rw = p->value("rw", 0);
        v.effective.max = p->value("max", -1L);
    }
    return v;
}

const char *obs_watch_dir_word(int is_write) {
    // Three outcomes, three words. "undecodable" means the engine saw the
    // access and could not decode the faulting instruction — which is a fact
    // about the trap, not a missing read or a missing write.
    return is_write == 1 ? "write" : (is_write == 0 ? "read" : "undecodable");
}

std::string obs_watch_value_cell(const WatchHit &h) {
    if (!h.value_ok)
        return "(not read back)";
    char b[64];
    std::snprintf(b, sizeof b, "0x%0*llx", static_cast<int>(h.value_len * 2),
                  static_cast<unsigned long long>(h.value));
    return std::string(b) + " (" + std::to_string(h.value_len) + "B)";
}

std::string obs_watch_loc(const WatchHit &h) {
    if (!h.have_loc || h.func.empty())
        return hex(h.pc);
    std::string s = h.func;
    if (h.off)
        s += "+" + hex(h.off);
    if (!h.module.empty())
        s += " [" + h.module + "]";
    return s;
}

std::string obs_watch_arm_error(const WatchArm &a) {
    // Verbatim the serve loop's flag matrix (cli/asmspy.c), so the client and
    // the server refuse with the same sentence.
    if (!a.addr)
        return "mode \"watch\" needs \"addr\"";
    if (a.len != 1 && a.len != 2 && a.len != 4 && a.len != 8)
        return "\"len\" must be 1, 2, 4 or 8 for mode \"watch\"";
    if (a.addr % static_cast<uint64_t>(a.len))
        return "\"addr\" must be \"len\"-aligned (an x86 hardware rule)";
    if (a.rw != 0 && a.rw != 1)
        return "\"rw\" must be 0 (writes) or 1 (reads and writes)";
    return "";
}

std::string obs_watch_start_command(const WatchArm &a, long pid) {
    if (!obs_watch_arm_error(a).empty())
        return "";
    nlohmann::json out = nlohmann::json::object();
    out["cmd"] = "start";
    out["mode"] = "watch";
    out["pid"] = pid;
    out["addr"] = a.addr;
    out["len"] = a.len;
    out["rw"] = a.rw;
    if (a.max >= 0)
        out["max"] = a.max;
    return out.dump();
}

std::string obs_watch_dump(const WatchView &v) {
    std::string s;
    if (!v.chrome.banner.empty())
        s += "banner=" + v.chrome.banner + "\n";
    s += "chrome=" + obs_chrome_chip(v.chrome) + "\n";
    if (v.have_effective)
        s += "armed: addr=" + hex(v.effective.addr) +
             " len=" + std::to_string(v.effective.len) +
             " rw=" + std::to_string(v.effective.rw) +
             " max=" + std::to_string(v.effective.max) + "\n";
    if (v.skip.present) {
        // The refusal, as measured. A skip is a successful session that had
        // nothing to report, so it is never spelled like a failure.
        s += "REFUSED (" + std::to_string(v.skip.code) + "): " + v.skip.reason +
             "\n";
    }
    s += "hits=" + std::to_string(v.hits.size()) + "\n";
    for (const WatchHit &h : v.hits)
        s += "  #" + std::to_string(h.hit_no) +
             " tid=" + std::to_string(h.tid) + " " +
             obs_watch_dir_word(h.is_write) + " @" + hex(h.addr) + " from " +
             obs_watch_loc(h) + " value=" + obs_watch_value_cell(h) + "\n";
    return s;
}

} // namespace asmdesk
