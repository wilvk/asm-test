// session.h — the desktop's live-capture host (07-serve-live-host.md T3).
//
// The desktop NEVER links the ptrace engines (D9). It captures by spawning
// `asmspy --serve` as a SUBPROCESS — locally, or as `ssh <host> asmspy --serve`
// — and speaking the NDJSON control protocol over its pipes. That one seam buys
// three things at once: local and remote capture are the same code path, a
// render-only viewer with zero engine deps can still host live sessions, and
// the tracer's hard-won guarantees (one tracer thread, two-phase detach) stay
// inside the tested binary that owns them.
//
// Protocol: docs/internal/gui/asmtrace-schema.md, "Serve protocol".
//
// The class is split so the INTERESTING half needs no subprocess: feed_line()
// and mark_eof() are a pure state machine over the wire format, and everything
// process-shaped (fork/exec/pipes/reap) only decides when to call them. Tests
// drive both — the state machine directly, and the process path against a fake
// serve script.
#ifndef ASMDESK_LIVE_SESSION_H
#define ASMDESK_LIVE_SESSION_H

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "doc/recording.h"

namespace asmdesk {

// Where the host is in the protocol. This is the SESSION's state, not the
// subprocess's: a serve host with no session running is Idle, not stopped.
enum class LiveState {
    Idle,    // host up, no session running
    Running, // a `session started` arrived and no terminal event yet
    Ended,   // the host is gone (exited, or we quit it)
    Failed,  // the host could not be started at all
};

// One lifecycle line off the wire (`session` / `cmd` / `err`), kept in order so
// the UI can show what was asked for and what came back — including refusals,
// which are the thing a client most needs to see and most easily drops.
struct LiveNote {
    std::string kind; // "session" | "cmd" | "err"
    nlohmann::json body;
};

struct LiveStatus {
    LiveState state = LiveState::Idle;
    std::string mode;    // the running mode, "" when idle
    long pid = 0;        // the traced pid of the running session
    std::string command; // what was spawned, for the UI to show

    // The last refusal. An `err` never ends a session (protocol), so this is
    // informational — but it must be SHOWN, not swallowed.
    std::string last_err;

    // The last terminal session event. `skip_code` != 0 means the session
    // succeeded and had nothing to report — never an error.
    std::string last_stop_reason;
    int skip_code = 0;
    std::string skip_reason;
    uint64_t paused_dropped = 0;

    uint64_t sessions_started = 0;
    uint64_t sessions_ended = 0;

    bool host_exited = false; // the subprocess is gone
    int host_status = 0;      // its wait status, when it exited
    std::string fatal;        // why we are Failed
};

class LiveSession {
  public:
    // How to reach a serve host.
    struct Spec {
        // "" = find one: `asmspy` on $PATH, then ./build/asmspy. Naming a path
        // here overrides both.
        std::string asmspy_path;
        // non-empty: run it as `ssh <host> <asmspy> --serve` instead of
        // locally. The remote transport IS ssh — the protocol carries no auth
        // of its own, by design.
        std::string ssh_host;
        // non-empty: spawn the host with `--record=<path>`, teeing every event
        // of EVERY capture in this session into one file.
        //
        // This is the only artifact that can carry `call` + `trace`/`coverage`
        // + wide `df_step.ops` together, because LiveSession keeps each capture
        // as its own Recording (`done_` is a vector) and the Save pane
        // serialises exactly one of them. A merged file reopened through
        // File ▸ Open is what lets the 3D pane host four substrates at once.
        //
        // Over ssh the path is written on the REMOTE host — `start` refuses the
        // combination rather than producing a file the user cannot open.
        std::string record_path;
    };

    // The exact argv the host is spawned with, for `exe` already resolved.
    // Pure, so a test pins both the presence and the ABSENCE of --record
    // without spawning anything: an empty record_path must leave the argv
    // byte-identical to what every existing lane already expects.
    static std::vector<std::string> host_argv(const Spec &spec,
                                              const std::string &exe);

    LiveSession() = default;
    ~LiveSession();
    LiveSession(const LiveSession &) = delete;
    LiveSession &operator=(const LiveSession &) = delete;

    // Spawn the host. Returns false with `err` set; the status goes Failed.
    bool start(const Spec &spec, std::string &err);

    // Pump once per UI frame. Non-blocking: reads whatever is available,
    // feeds every COMPLETE line, and reaps the host if it exited.
    void poll();

    // Send one command line (a newline is appended if absent). No-op when the
    // host is gone — a caller must not have to guard every send.
    void send(const std::string &json_line);

    // Convenience wrappers over the protocol's commands.
    void send_start(const std::string &mode, long pid,
                    const nlohmann::json &params = nlohmann::json::object());
    // docs/internal/archive/gui/45-launch-and-window-target.md T3: `launch` — same
    // shape as send_start, but there is no pid yet (the host forks one); the
    // command carries `argv` (argv[0] is the path to run) and an optional
    // `cwd` instead. The resulting pid arrives later on the `session started`
    // event and is readable from status().pid exactly as an attach's is —
    // feed_line() already sets it from that event's "pid" field regardless of
    // which command produced it, so no separate plumbing is needed here.
    void send_launch(const std::string &mode, const std::vector<std::string> &argv,
                     const std::string &cwd,
                     const nlohmann::json &params = nlohmann::json::object());
    void send_pause(bool on);
    void send_stop();

    // `quit`, then reap. Safe to call twice.
    void shutdown();

    // Drop every recording, note, and counter, returning the session to its
    // just-constructed state. Precondition: the host is down — call shutdown()
    // first. reset() deliberately does not touch the pipes, so calling it with
    // a host still up would orphan an fd; it clears only the accumulated wire
    // state. This is what makes a Disconnect -> Connect a genuinely fresh
    // session rather than one still haunted by the previous host's recordings.
    void reset();

    // Drop the COMPLETED recordings and the notes that accompanied them,
    // keeping the host, the pipes, the status and any still-growing capture
    // — the "clear previous sessions" affordance (2026-08-10 simplified-LOD
    // spec): the union weave and every accumulating scene start again from
    // what is still live. Returns false and does NOTHING while a capture is
    // growing: "previous" means FINISHED, and refusing while open keeps the
    // lifecycle notes trivially attributable (the current capture's notes
    // are never guessed apart from history's). reset() remains the
    // Disconnect-only full teardown; the malformed-line counter survives —
    // it is wire health, not capture data.
    bool clear_completed();

    const LiveStatus &status() const { return st_; }
    // Completed recordings, oldest first — one per finished mode session.
    const std::vector<Recording> &recordings() const { return done_; }
    // The recording currently growing, or nullptr between sessions.
    const Recording *growing() const { return open_ ? &cur_ : nullptr; }
    const std::vector<LiveNote> &notes() const { return notes_; }

    // ---- the state machine (no subprocess needed; this is what tests drive) --
    // One complete wire line. Anything unparseable is COUNTED, never fatal: a
    // live host is not a file, and one bad line must not discard a session.
    void feed_line(const std::string &line);
    // The stream ended. An open recording becomes TORN — the schema's rule,
    // and the faithful reading of a producer that stopped mid-record.
    void mark_eof();
    uint64_t malformed_lines() const { return malformed_; }

  private:
    void close_current(bool torn);
    void reap(bool blocking);

    LiveStatus st_;
    std::vector<Recording> done_;
    std::vector<LiveNote> notes_;
    Recording cur_;
    bool open_ = false; // a header arrived and no `end` has yet
    uint64_t malformed_ = 0;

    std::string inbuf_; // partial line carried between polls
    int rfd_ = -1;
    int wfd_ = -1;
    int child_ = -1;
};

// The `asmspy` a Spec with no explicit path resolves to, exposed for the UI to
// show and for tests to assert. Returns "" when neither candidate exists.
std::string resolve_asmspy_path();

} // namespace asmdesk
#endif // ASMDESK_LIVE_SESSION_H
