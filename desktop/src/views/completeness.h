// completeness.h — the backend-completeness panel's state + draw
// (docs/internal/gui/02-exporters-and-readers.md T6). Every RULE lives in
// completeness_model.h (pure, tested); this is the panel's chrome only.
#ifndef ASMDESK_VIEWS_COMPLETENESS_H
#define ASMDESK_VIEWS_COMPLETENESS_H

#include <string>
#include <vector>

#include "data/perf_history.h"
#include "views/completeness_model.h"

namespace asmdesk {

// Which record is showing: a committed box, or a features file the user pointed
// at. The panel NEVER runs a sweep of its own — `make features` does that, and a
// view that silently probed would be reporting on a different machine than the
// box row it is drawn under.
struct CompletenessState {
    std::vector<data::BoxRecord> boxes;
    int selected = -1;
    char custom_path[1024] = {0};
    std::string error; // a load failure, rendered verbatim
    CompletenessTable table;
    bool scanned = false;
};

void draw_completeness(CompletenessState &s, const std::string &repo_root);

} // namespace asmdesk
#endif // ASMDESK_VIEWS_COMPLETENESS_H
