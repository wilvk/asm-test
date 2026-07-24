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
    };

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

    // Convenience wrappers over the protocol's four commands.
    void send_start(const std::string &mode, long pid,
                    const nlohmann::json &params = nlohmann::json::object());
    void send_pause(bool on);
    void send_stop();

    // `quit`, then reap. Safe to call twice.
    void shutdown();

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
    // and the honest reading of a producer that stopped mid-record.
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
