// workspace_state.cpp — see workspace_state.h (20-workspace-and-settings.md T3/T4).
#include "doc/workspace_state.h"

#include <nlohmann/json.hpp>

namespace asmdesk {

using nlohmann::json;

std::string workspace_state_serialize(const WorkspaceState &s) {
    json j;
    j["open"] = s.open;
    j["active"] = s.active;
    j["pane_links"] = s.pane_links;
    j["recents"] = s.recents;
    j["perspectives"] = s.perspectives;
    json presets = json::array();
    for (const FilterPreset &p : s.presets)
        presets.push_back({{"name", p.name}, {"query", p.query}});
    j["presets"] = presets;
    // Two-space pretty print: the store is meant to be diffable in a review.
    return j.dump(2);
}

bool workspace_state_parse(const std::string &text, WorkspaceState &out) {
    out = WorkspaceState{}; // defaults; a bad parse leaves exactly this
    json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object())
        return false;

    // Each field is read defensively: a wrong type for one key must not lose the
    // others, and an unknown key is simply ignored (forward-compat).
    auto str_vec = [](const json &v, std::vector<std::string> &dst) {
        if (!v.is_array())
            return;
        for (const json &e : v)
            if (e.is_string())
                dst.push_back(e.get<std::string>());
    };
    if (auto it = j.find("open"); it != j.end())
        str_vec(*it, out.open);
    if (auto it = j.find("active"); it != j.end() && it->is_string())
        out.active = it->get<std::string>();
    if (auto it = j.find("pane_links"); it != j.end())
        str_vec(*it, out.pane_links);
    if (auto it = j.find("recents"); it != j.end())
        str_vec(*it, out.recents);
    if (auto it = j.find("perspectives"); it != j.end() && it->is_object())
        for (auto &kv : it->items())
            if (kv.value().is_string())
                out.perspectives[kv.key()] = kv.value().get<std::string>();
    if (auto it = j.find("presets"); it != j.end() && it->is_array())
        for (const json &e : *it) {
            if (!e.is_object())
                continue;
            FilterPreset p;
            if (auto n = e.find("name"); n != e.end() && n->is_string())
                p.name = n->get<std::string>();
            if (auto q = e.find("query"); q != e.end() && q->is_string())
                p.query = q->get<std::string>();
            if (!p.name.empty())
                out.presets.push_back(std::move(p));
        }
    return true;
}

void recents_push(std::vector<std::string> &recents, const std::string &path,
                  size_t cap) {
    if (path.empty())
        return;
    // Move-to-front: drop any existing copy, then prepend, then cap the tail.
    for (auto it = recents.begin(); it != recents.end();) {
        if (*it == path)
            it = recents.erase(it);
        else
            ++it;
    }
    recents.insert(recents.begin(), path);
    if (cap > 0 && recents.size() > cap)
        recents.resize(cap);
}

} // namespace asmdesk
