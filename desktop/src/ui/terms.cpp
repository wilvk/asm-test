// terms.cpp — the in-app term registry (see terms.h). The registry table is
// GENERATED from docs/project/glossary.md by scripts/gen-terms.py; this TU only
// #includes that generated header and wraps it. No second, hand-copied word list.
#include "ui/terms.h"

#include <cctype>
#include <cstring>

#include "ui/theme.h"

// The one generated table (k_dt_terms / k_dt_terms_n). Its build rule (mk/
// desktop.mk) regenerates it from the glossary before this TU compiles.
#include "ui/terms_generated.h"

#ifdef ASMDESK_HAVE_IMSEARCH
#include "imsearch.h"
#endif

namespace asmdesk {

int dt_terms_count() { return k_dt_terms_n; }
const char *dt_term_name(int i) {
    return (i >= 0 && i < k_dt_terms_n) ? k_dt_terms[i].term : "";
}
const char *dt_term_def(int i) {
    return (i >= 0 && i < k_dt_terms_n) ? k_dt_terms[i].def : "";
}

namespace {
bool ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (std::tolower((unsigned char)*a) != std::tolower((unsigned char)*b))
            return false;
    return *a == *b;
}
} // namespace

std::string dt_term_lookup(const char *word) {
    if (word == nullptr)
        return "";
    for (int i = 0; i < k_dt_terms_n; i++)
        if (ieq(k_dt_terms[i].term, word))
            return k_dt_terms[i].def;
    return "";
}

// --- per-view metadata (one table) -------------------------------------------
// Domain term leads; metaphor is the subtitle; caveat is the verbatim honest
// note the "?" popover shows. The coined surfaces of shell.cpp's tab strip.
static const dt_view_meta k_meta[] = {
    {"loom", "Data-flow lineage", "Loom",
     "Lineage is def-use, not control flow: a worldline shows where a value "
     "came from and what it fed, never the order instructions ran."},
    {"scene3d", "Address-space spacetime", "3D overview",
     "Terrain is the address space, a trajectory is one execution path; exact "
     "paths are solid tubes, statistical residency is stippled. 3D to find, 2D "
     "to read."},
    {"hotedges", "Edge execution counts", "hot-edges",
     "Hot-edges are edge counts, not a call stack and not an ordered path. "
     "Statistical when the source is a sampled backend."},
    {"slice", "Dependency slice", "cone explorer",
     "The back-cone is what produced the selected value; the fwd-cone is what "
     "it goes on to affect. Def-use edges, not a timeline."},
    {"diff", "First divergence", "patient zero",
     "Patient zero is the earliest step at which two recordings stop agreeing "
     "— the root of everything downstream that differs."},
    {"scrubber", "Register time-travel", "scrubber",
     "Each step shows the register file AS RECORDED; an absent producer draws a "
     "placard, never a register file of zeros."},
    {"abixray", "Calling-convention x-ray", "ABI x-ray",
     "Two legs of one routine (SysV and Win64) locked to one playhead; an "
     "unaligned pair is drawn as a refusal, never as agreement."},
    {"terms", "Terminology", "Terms",
     "Every coined GUI term, defined from the one glossary the docs use."},
};
static const int k_meta_n = (int)(sizeof(k_meta) / sizeof(k_meta[0]));

int dt_view_meta_count() { return k_meta_n; }
const dt_view_meta *dt_view_meta_at(int i) {
    return (i >= 0 && i < k_meta_n) ? &k_meta[i] : nullptr;
}
const dt_view_meta *dt_view_meta_get(const char *key) {
    if (key == nullptr)
        return nullptr;
    for (int i = 0; i < k_meta_n; i++)
        if (std::strcmp(k_meta[i].key, key) == 0)
            return &k_meta[i];
    return nullptr;
}

// --- draw helpers ------------------------------------------------------------
void dt_view_heading(const char *domain, const char *metaphor) {
    ImGui::TextUnformatted(domain);
    if (metaphor != nullptr && metaphor[0] != '\0') {
        ImGui::SameLine();
        ImGui::TextColored(dt_dim_col(), "(%s)", metaphor);
    }
}

void dt_term(const char *word) {
    ImGui::TextUnformatted(word);
    std::string def = dt_term_lookup(word);
    if (!def.empty() && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
        ImGui::TextUnformatted(def.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

bool dt_view_help_button(const char *key,
                         const std::function<void()> &on_reopen_primer) {
    const dt_view_meta *m = dt_view_meta_get(key);
    ImGui::PushID(key);
    bool clicked = ImGui::SmallButton("?");
    if (clicked)
        ImGui::OpenPopup("view-caveat");
    if (ImGui::BeginPopup("view-caveat")) {
        if (m != nullptr) {
            dt_view_heading(m->domain, m->metaphor);
            ImGui::Separator();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
            ImGui::TextUnformatted(m->caveat);
            ImGui::PopTextWrapPos();
        }
        if (on_reopen_primer) {
            ImGui::Separator();
            if (ImGui::SmallButton("show primer again")) {
                on_reopen_primer();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return clicked;
}

void dt_view_header(const char *key,
                    const std::function<void()> &on_reopen_primer) {
    const dt_view_meta *m = dt_view_meta_get(key);
    if (m == nullptr)
        return;
    dt_view_heading(m->domain, m->metaphor);
    ImGui::SameLine();
    dt_view_help_button(key, on_reopen_primer);
}

void dt_terms_pane() {
    ImGui::TextDisabled("%d term(s) — the GUI's coined lexicon, from the one "
                        "glossary",
                        dt_terms_count());
#ifdef ASMDESK_HAVE_IMSEARCH
    if (ImSearch::GetCurrentContext() && ImSearch::BeginSearch()) {
        ImSearch::SearchBar("filter terms");
        for (int i = 0; i < dt_terms_count(); i++) {
            const char *name = dt_term_name(i);
            const char *def = dt_term_def(i);
            ImSearch::SearchableItem(name, [name, def](const char *) {
                ImGui::Bullet();
                ImGui::TextUnformatted(name);
                ImGui::SameLine();
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(dt_dim_col(), "— %s", def);
                ImGui::PopTextWrapPos();
            });
        }
        ImSearch::Submit();
        ImSearch::EndSearch();
        return;
    }
#endif
    // Null backend / no ImSearch: a plain scrollable list.
    for (int i = 0; i < dt_terms_count(); i++) {
        ImGui::Bullet();
        ImGui::TextUnformatted(dt_term_name(i));
        ImGui::SameLine();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(dt_dim_col(), "— %s", dt_term_def(i));
        ImGui::PopTextWrapPos();
    }
}

} // namespace asmdesk
