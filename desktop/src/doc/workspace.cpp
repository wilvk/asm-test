// workspace.cpp — see workspace.h. 03-desktop-shell.md T3.
#include "doc/workspace.h"

namespace asmdesk {

int Workspace::open(const std::string &path, std::string &err) {
    std::optional<Recording> rec = load_recording_file(path, err);
    if (!rec)
        return -1;
    recordings.push_back(std::move(*rec));
    return static_cast<int>(recordings.size()) - 1;
}

void Workspace::close(size_t idx) {
    if (idx < recordings.size())
        recordings.erase(recordings.begin() + static_cast<std::ptrdiff_t>(idx));
}

} // namespace asmdesk
