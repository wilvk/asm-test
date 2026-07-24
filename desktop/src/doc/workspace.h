// workspace.h — the set of open recordings the desktop renders (plan D3: the
// document model is a SET, and every view accepts one or two of them).
// 03-desktop-shell.md T3.
#ifndef ASMDESK_DOC_WORKSPACE_H
#define ASMDESK_DOC_WORKSPACE_H

#include <cstddef>
#include <string>
#include <vector>

#include "doc/recording.h"

namespace asmdesk {

struct Workspace {
    std::vector<Recording> recordings;

    // Open a recording and add it to the set. Returns its index on success, or
    // -1 with `err` set (the loader's message) on any reject/IO failure — never a
    // silent no-op.
    int open(const std::string &path, std::string &err);
    // Close the recording at `idx` (no-op if out of range).
    void close(size_t idx);
};

} // namespace asmdesk
#endif // ASMDESK_DOC_WORKSPACE_H
