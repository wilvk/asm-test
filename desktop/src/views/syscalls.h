// syscalls.h — the live/replay syscall stream (08-observer-views.md T1).
//
// The schema splits every syscall in two: `line` is the payload-FREE rendering
// (name, fds, flag words, counts, return value — with `<path>` / `<N bytes>`
// placeholders where content would be), and the decoded content travels
// separately in `payload`. That split exists so this view can be honest about
// two different things at once: what the target did, always; and what it did it
// WITH, only when someone asked.
//
// So the payload column is REDACTED BY DEFAULT (D7), per row, and revealing is
// an action — never a setting that quietly persists across a session, and never
// something a view does for you because the bytes happened to be there. The
// session-wide reveal takes a second confirmation for the same reason a
// destructive button does: "show me every path, sockaddr and buffer this
// process touched" is a decision, not a preference.
//
// Two things this view deliberately does NOT offer:
//
//  - **tid pinning.** The syscalls engine takes no `only_tid` parameter
//    (cli/libasmspy.h) — the engine tags a multi-threaded stream by prefixing
//    "[tid] " to the line itself. A filter box here would be a client-side
//    illusion over an engine that never stopped following every thread, so the
//    UI states the fact instead of offering the control.
//  - **un-redacting the recording.** Reveal is a RENDERER act. It never mutates
//    the recording, and when the producer redacted at record time the bytes are
//    not in the file at all — a distinction the cell text keeps.
#ifndef ASMDESK_VIEWS_SYSCALLS_H
#define ASMDESK_VIEWS_SYSCALLS_H

#include <cstddef>
#include <string>
#include <vector>

#include "doc/recording.h"
#include "views/observer.h"

namespace asmdesk {

struct SyscallRow {
    size_t index = 0;
    std::string line;         // payload-free by schema; safe to show always
    bool has_payload = false; // a `payload` field was on the wire
    std::string payload;      // the content. NEVER rendered unless revealed.
    long tid = -1;            // v1 writers omit `tid`; -1 = absent
};

struct SyscallView {
    std::vector<SyscallRow> rows;
    ObsChrome chrome;

    // The producer withheld payloads AT RECORD TIME (provenance.redacted): the
    // content is not in this file at all. Materially different from "hidden by
    // this renderer", and the cell text must not blur the two.
    bool record_redacted = false;

    // Parallel to `rows`. Reveal state is per row and lives here, not in the
    // document — nothing about looking at a payload changes the recording.
    std::vector<char> revealed;
    bool reveal_all = false;
    bool reveal_all_armed = false; // a first click arms; a second confirms

    // The `--follow` toggle: whether the live session follows child processes.
    // Recorded here so the view can show what the session was started with.
    bool follow = false;
};

// `lc` overrides where the session lifecycle is read from: a LIVE session
// keeps it outside the recording (see ObsLifecycle). nullptr = the
// recording's own, which is what a replayed file carries.
SyscallView obs_syscalls_build(const Recording &r,
                               const ObsLifecycle *lc = nullptr);

// The payload cell's text — the rule this whole view exists to get right, so it
// is one pure function with one test rather than a branch inside a draw call.
// Never returns payload bytes unless that row is revealed.
std::string obs_syscall_payload_cell(const SyscallView &v, size_t i);

void obs_syscall_reveal(SyscallView &v, size_t i, bool on);

// The session-wide reveal. The FIRST call only arms the confirmation (and
// returns false); a second call within the armed state performs it. Any other
// interaction should clear `reveal_all_armed`.
bool obs_syscall_reveal_all(SyscallView &v);
// The confirmation prompt, naming the consequence rather than asking "are you
// sure?" — which tells a reader nothing they did not already know.
std::string obs_syscall_reveal_all_prompt(const SyscallView &v);

// Why this view offers no tid filter (the engine fact above), for the UI to
// show in place of the control it is not going to draw.
const char *obs_syscall_tid_note();

std::string obs_syscalls_dump(const SyscallView &v);

} // namespace asmdesk
#endif // ASMDESK_VIEWS_SYSCALLS_H
