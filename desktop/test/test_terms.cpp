// test_terms.cpp — the in-app term registry, driven from the ONE glossary
// (24-one-visual-language.md T3 / F3). Model/source assertions, no pixels (D4):
//  (a) the registry is SOURCED from docs/project/glossary.md — parse the same
//      file the build step parses and assert every headword resolves in the
//      generated table (one source, no hand-copied second list);
//  (b) a coined GUI term resolves to a definition carrying its expert synonym,
//      and every coined term added in step 1 of the task is present (the F3
//      completeness gap is closed);
//  (c) every registered view exposes a non-empty per-view "?" caveat and a
//      heading with both a domain term and a metaphor.
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "ui/terms.h"

#ifndef ASMTEST_GLOSSARY_MD
#error "ASMTEST_GLOSSARY_MD must be defined by the build (mk/desktop.mk)"
#endif

using namespace asmdesk;

static int failures;
static void check(const char *what, bool cond, const std::string &why) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s: %s\n", what, why.c_str());
        failures++;
    }
}

// Parse the glossary headwords the same way scripts/gen-terms.py does: within the
// :::{glossary} ... ::: block, a headword is a non-indented, non-blank line that
// is not a directive option (":...").
static std::vector<std::string> glossary_headwords(const std::string &path) {
    std::vector<std::string> out;
    std::ifstream f(path);
    std::string line;
    bool in_block = false;
    while (std::getline(f, line)) {
        std::string t = line;
        // trim trailing CR/space for the block markers
        while (!t.empty() && (t.back() == '\r' || t.back() == ' '))
            t.pop_back();
        if (!in_block) {
            if (t.find(":::{glossary}") != std::string::npos)
                in_block = true;
            continue;
        }
        if (t == ":::")
            break;
        if (line.empty())
            continue;
        if (line[0] == ' ' || line[0] == '\t')
            continue; // definition line
        if (line[0] == ':')
            continue; // directive option (:sorted:)
        out.push_back(t);
    }
    return out;
}

int main() {
    // --- (a) one source: every glossary headword resolves in the registry -----
    std::vector<std::string> heads = glossary_headwords(ASMTEST_GLOSSARY_MD);
    check("glossary-parsed", heads.size() > 20,
          "expected a populated glossary, got " + std::to_string(heads.size()));
    check("registry-nonempty", dt_terms_count() > 0, "the registry is empty");
    for (const std::string &h : heads)
        check("headword-resolves", !dt_term_lookup(h.c_str()).empty(),
              "glossary headword '" + h +
                  "' is absent from the generated registry (one-source drift)");

    // --- (b) the coined GUI terms are present, with expert synonyms -----------
    struct Coined {
        const char *term;
        const char *synonym;
    };
    const Coined coined[] = {
        {"Loom", "def-use"},        {"fabric", "lineage"},
        {"patch-bay", "contention"}, {"hollow span", "route"},
        {"born-untraced", "outside"}, {"patient-zero", "divergence"},
        {"hot-edges", "count"},     {"knot", "join"},
        {"jack", "attachment"},     {"worldline", "def-use"},
        {"Reweave", "layout"},      {"hot take", "used"},
        {"dim take", "unread"},     {"terrane", "region"},
    };
    for (const Coined &c : coined) {
        std::string def = dt_term_lookup(c.term);
        check("coined-present", !def.empty(),
              std::string("coined term '") + c.term +
                  "' is not defined in the glossary (F3 gap)");
        std::string low;
        for (char ch : def)
            low.push_back((char)std::tolower((unsigned char)ch));
        check("coined-synonym", low.find(c.synonym) != std::string::npos,
              std::string("coined term '") + c.term +
                  "' definition lacks its expert synonym '" + c.synonym + "'");
    }

    // --- (c) every registered view has a domain, metaphor and caveat ----------
    check("view-meta-nonempty", dt_view_meta_count() > 0,
          "no per-view metadata registered");
    for (int i = 0; i < dt_view_meta_count(); i++) {
        const dt_view_meta *m = dt_view_meta_at(i);
        check("view-domain", m && m->domain && m->domain[0],
              "a view is missing its domain term");
        check("view-metaphor", m && m->metaphor && m->metaphor[0],
              std::string("view '") + (m ? m->key : "?") +
                  "' is missing its metaphor subtitle");
        check("view-caveat", m && m->caveat && m->caveat[0],
              std::string("view '") + (m ? m->key : "?") +
                  "' is missing its per-view '?' caveat");
    }
    // The coined surfaces the task names each carry a caveat.
    const char *keys[] = {"loom", "scene3d", "hotedges"};
    for (const char *k : keys)
        check("key-registered", dt_view_meta_get(k) != nullptr,
              std::string("view '") + k + "' has no registered metadata");

    if (failures) {
        std::fprintf(stderr, "%d test_terms check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_terms: all checks passed\n");
    return 0;
}
