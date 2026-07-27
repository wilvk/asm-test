// test_workspace_state.cpp — the persisted workspace store
// (20-workspace-and-settings.md T3/T4). Pure model, no ImGui, no display: assert
// the store round-trips byte-stably through serialise->parse (open set + active
// + pane links + recents + perspectives + presets), that recents_push dedups /
// orders / caps, and that a corrupt store degrades to defaults rather than
// crashing (parse defensively).
#include <cstdio>
#include <string>

#include "doc/workspace_state.h"

using namespace asmdesk;

static int failures;
static void check(const char *what, bool cond, const char *why) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s: %s\n", what, why);
        failures++;
    }
}

int main() {
    // --- round-trip ---------------------------------------------------------
    WorkspaceState ws;
    ws.open = {"a.asmtrace", "b.asmtrace"};
    ws.active = "asmtrace-link:v=timeline&rec=b.asmtrace&step=4";
    ws.pane_links = {"asmtrace-link:v=canvas&rec=a.asmtrace",
                     "asmtrace-link:v=timeline&rec=b.asmtrace&step=4"};
    ws.recents = {"b.asmtrace", "a.asmtrace"};
    ws.perspectives["Replay / Inspect"] = "preset:Replay / Inspect";
    ws.perspectives["mine"] = "ini:[Docking][Data]\npos=0,0\n";
    ws.presets.push_back({"reads", "read"});
    ws.presets.push_back({"opens", "open*"});

    std::string json = workspace_state_serialize(ws);
    WorkspaceState back;
    check("round-trips", workspace_state_parse(json, back),
          "the serialised store must parse");
    check("open", back.open == ws.open, "open set drifted");
    check("active", back.active == ws.active, "active position drifted");
    check("pane_links", back.pane_links == ws.pane_links, "pane links drifted");
    check("recents", back.recents == ws.recents, "recents drifted");
    check("perspectives", back.perspectives == ws.perspectives,
          "perspectives drifted");
    check("presets-count", back.presets.size() == 2, "preset count drifted");
    check("preset-fields",
          back.presets.size() == 2 && back.presets[0].name == "reads" &&
              back.presets[0].query == "read" &&
              back.presets[1].name == "opens" &&
              back.presets[1].query == "open*",
          "a preset's name/query drifted");

    // Byte-stable: serialising the parsed copy yields the same text.
    check("byte-stable", workspace_state_serialize(back) == json,
          "serialise(parse(x)) must be byte-identical");

    // --- recents_push: dedup, order (most-recent-first), cap ----------------
    {
        std::vector<std::string> r;
        recents_push(r, "one");
        recents_push(r, "two");
        recents_push(r, "one"); // re-touch moves it to front, no duplicate
        check("recents/dedup", r.size() == 2, "a re-touch must not duplicate");
        check("recents/order", r[0] == "one" && r[1] == "two",
              "the most recent must be first");
        // Cap: push 20 distinct with cap 12 -> only the 12 most recent survive.
        std::vector<std::string> big;
        for (int i = 0; i < 20; i++)
            recents_push(big, "p" + std::to_string(i), 12);
        check("recents/cap", big.size() == 12, "recents must cap at 12");
        check("recents/cap-front", big.front() == "p19",
              "the newest push is at the front after capping");
        // An empty path is ignored.
        recents_push(big, "", 12);
        check("recents/empty-ignored", big.size() == 12,
              "an empty path must not be pushed");
    }

    // --- defensive parse: garbage degrades to defaults, never crashes -------
    {
        WorkspaceState out;
        check("parse/garbage-false", !workspace_state_parse("{ not json", out),
              "malformed JSON must return false");
        check("parse/garbage-default", out.open.empty() && out.active.empty(),
              "a bad parse must leave defaults");
        check("parse/non-object-false", !workspace_state_parse("[1,2,3]", out),
              "a non-object must return false");
        // A partial store keeps what it can (forward-compat + a wrong type on one
        // key must not lose the others).
        WorkspaceState partial;
        check("parse/partial-ok",
              workspace_state_parse(
                  "{\"open\":[\"x\"],\"active\":42,\"future\":true}", partial),
              "an object parses even with a wrong-typed field + unknown key");
        check("parse/partial-open", partial.open.size() == 1 &&
                                        partial.open[0] == "x",
              "the good field survives");
        check("parse/partial-active", partial.active.empty(),
              "a wrong-typed field is ignored, not fatal");
    }

    if (failures) {
        std::fprintf(stderr, "test_workspace_state: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("test_workspace_state: all checks passed\n");
    return 0;
}
