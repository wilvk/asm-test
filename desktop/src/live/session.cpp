// session.cpp — spawn `asmspy --serve` and turn its stream into Recordings
// (07-serve-live-host.md T3). See session.h for why the capture host is a
// subprocess and not a linked library (D9).
#include "live/session.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <sstream>

#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace asmdesk {

using nlohmann::json;

namespace {

json parse_line(const std::string &line) {
    return json::parse(line, /*cb=*/nullptr, /*allow_exceptions=*/false);
}

uint64_t as_u64(const json &v, uint64_t dflt) {
    return v.is_number() ? v.get<uint64_t>() : dflt;
}

bool is_executable(const std::string &p) {
    return !p.empty() && ::access(p.c_str(), X_OK) == 0;
}

} // namespace

// $PATH first, then the in-tree build — the order a developer expects, and the
// only two places a working asmspy is ever found without being told.
std::string resolve_asmspy_path() {
    const char *path = ::getenv("PATH");
    if (path) {
        std::string p(path), dir;
        size_t start = 0;
        while (start <= p.size()) {
            size_t sep = p.find(':', start);
            if (sep == std::string::npos)
                sep = p.size();
            dir = p.substr(start, sep - start);
            if (!dir.empty() && is_executable(dir + "/asmspy"))
                return dir + "/asmspy";
            start = sep + 1;
        }
    }
    if (is_executable("./build/asmspy"))
        return "./build/asmspy";
    return "";
}

LiveSession::~LiveSession() { shutdown(); }

bool LiveSession::start(const Spec &spec, std::string &err) {
    err.clear();
    std::string exe = spec.asmspy_path;
    if (exe.empty() && spec.ssh_host.empty()) {
        exe = resolve_asmspy_path();
        if (exe.empty()) {
            err = "no asmspy found on $PATH or at ./build/asmspy — build it "
                  "with `make cli`, or name one explicitly";
            st_.state = LiveState::Failed;
            st_.fatal = err;
            return false;
        }
    }
    // Over ssh the REMOTE path is what matters and we cannot probe it, so an
    // unqualified name is right: it resolves in the remote $PATH.
    if (exe.empty())
        exe = "asmspy";

    std::vector<std::string> argv;
    if (spec.ssh_host.empty()) {
        argv = {exe, "--serve"};
    } else {
        // -T: no tty, so the remote asmspy sees a plain pipe and our NDJSON is
        // never mangled by line-discipline translation.
        argv = {"ssh", "-T", spec.ssh_host, exe, "--serve"};
    }
    st_.command.clear();
    for (size_t i = 0; i < argv.size(); i++)
        st_.command += (i ? " " : "") + argv[i];

    int to_child[2], from_child[2];
    if (::pipe(to_child) != 0) {
        err = std::string("pipe: ") + std::strerror(errno);
        st_.state = LiveState::Failed;
        st_.fatal = err;
        return false;
    }
    if (::pipe(from_child) != 0) {
        err = std::string("pipe: ") + std::strerror(errno);
        ::close(to_child[0]);
        ::close(to_child[1]);
        st_.state = LiveState::Failed;
        st_.fatal = err;
        return false;
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        err = std::string("fork: ") + std::strerror(errno);
        ::close(to_child[0]);
        ::close(to_child[1]);
        ::close(from_child[0]);
        ::close(from_child[1]);
        st_.state = LiveState::Failed;
        st_.fatal = err;
        return false;
    }
    if (pid == 0) {
        ::dup2(to_child[0], 0);
        ::dup2(from_child[1], 1);
        ::close(to_child[0]);
        ::close(to_child[1]);
        ::close(from_child[0]);
        ::close(from_child[1]);
        std::vector<char *> cargv;
        for (auto &a : argv)
            cargv.push_back(const_cast<char *>(a.c_str()));
        cargv.push_back(nullptr);
        ::execvp(cargv[0], cargv.data());
        // execvp failed. The parent learns from the immediate EOF + exit
        // status; writing here would corrupt the event stream.
        ::_exit(127);
    }

    ::close(to_child[0]);
    ::close(from_child[1]);
    wfd_ = to_child[1];
    rfd_ = from_child[0];
    child_ = pid;
    ::fcntl(rfd_, F_SETFL, O_NONBLOCK);
    // A host that dies mid-write must not take the UI down with it: the write
    // fails with EPIPE instead, and the session becomes torn.
    ::signal(SIGPIPE, SIG_IGN);

    st_.state = LiveState::Idle;
    st_.host_exited = false;
    st_.fatal.clear();
    return true;
}

void LiveSession::send(const std::string &json_line) {
    if (wfd_ < 0)
        return;
    std::string out = json_line;
    if (out.empty() || out.back() != '\n')
        out += '\n';
    size_t off = 0;
    while (off < out.size()) {
        ssize_t n = ::write(wfd_, out.data() + off, out.size() - off);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        break; // EPIPE / EAGAIN — the host is gone or wedged; poll() will tell
    }
}

void LiveSession::send_start(const std::string &mode, long pid,
                             const json &params) {
    json cmd = params.is_object() ? params : json::object();
    cmd["cmd"] = "start";
    cmd["mode"] = mode;
    cmd["pid"] = pid;
    send(cmd.dump());
}

void LiveSession::send_pause(bool on) {
    send(json{{"cmd", "pause"}, {"on", on}}.dump());
}

void LiveSession::send_stop() { send(json{{"cmd", "stop"}}.dump()); }

void LiveSession::poll() {
    if (rfd_ < 0) {
        reap(false);
        return;
    }
    char buf[8192];
    for (;;) {
        ssize_t n = ::read(rfd_, buf, sizeof buf);
        if (n > 0) {
            inbuf_.append(buf, (size_t)n);
            size_t nl;
            while ((nl = inbuf_.find('\n')) != std::string::npos) {
                feed_line(inbuf_.substr(0, nl));
                inbuf_.erase(0, nl + 1);
            }
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return; // nothing more this frame; the host is still up
        // n == 0 (EOF) or a hard read error: the host is finished.
        ::close(rfd_);
        rfd_ = -1;
        mark_eof();
        reap(true);
        return;
    }
}

void LiveSession::shutdown() {
    if (wfd_ >= 0) {
        send(R"({"cmd":"quit"})");
        ::close(wfd_);
        wfd_ = -1;
    }
    // Drain whatever the host emits on its way out — including the terminal
    // session event and the `end` footer, which are the difference between a
    // clean recording and a torn one.
    if (rfd_ >= 0) {
        for (int i = 0; i < 500 && rfd_ >= 0; i++) {
            poll();
            if (rfd_ < 0)
                break;
            struct timespec ts = {0, 10 * 1000 * 1000}; // 10ms
            ::nanosleep(&ts, nullptr);
        }
    }
    if (rfd_ >= 0) {
        ::close(rfd_);
        rfd_ = -1;
        mark_eof();
    }
    reap(true);
}

void LiveSession::reap(bool blocking) {
    if (child_ < 0)
        return;
    int status = 0;
    pid_t r = ::waitpid(child_, &status, blocking ? 0 : WNOHANG);
    if (r != child_)
        return;
    child_ = -1;
    st_.host_exited = true;
    st_.host_status = status;
    if (st_.state != LiveState::Failed)
        st_.state = LiveState::Ended;
    // 127 is execvp's own failure code: the host was never there to speak to,
    // which is a different problem from a host that ran and stopped.
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
        st_.state = LiveState::Failed;
        st_.fatal = "could not exec the serve host (" + st_.command + ")";
    }
}

void LiveSession::close_current(bool torn) {
    if (!open_)
        return;
    if (torn)
        cur_.torn = true;
    else if (!cur_.has_end)
        cur_.torn = true;
    done_.push_back(std::move(cur_));
    cur_ = Recording{};
    open_ = false;
}

void LiveSession::mark_eof() {
    // An open recording at EOF is TORN — the producer stopped mid-record, and
    // the reader's duty is to say so rather than present a prefix as complete.
    close_current(/*torn=*/true);
    if (st_.state == LiveState::Running) {
        // The stream died with a session still live: there is no terminal
        // event coming, so record why the UI is about to stop updating.
        st_.last_stop_reason = "host-eof";
        st_.state = LiveState::Ended;
    } else if (st_.state != LiveState::Failed) {
        st_.state = LiveState::Ended;
    }
    st_.mode.clear();
}

void LiveSession::feed_line(const std::string &line) {
    if (line.empty())
        return;
    json j = parse_line(line);
    if (j.is_discarded() || !j.is_object()) {
        // Counted, not fatal. A live host is not a file: one bad line must not
        // discard a session, and pretending we understood it would be worse.
        malformed_++;
        return;
    }

    // A header line opens a new recording (it carries `asmtrace`, not `k`).
    if (j.contains("asmtrace")) {
        // A header arriving with one still open means the previous session
        // never wrote its footer: that recording is torn, and saying so is the
        // whole point of having the field.
        close_current(/*torn=*/true);
        std::string err;
        // Reuse the file loader for the header's reject rules by handing it a
        // one-line stream: a newer major, a missing provenance or a defaulted
        // `exact` must be refused on the wire exactly as in a file.
        {
            std::istringstream one(line + "\n");
            std::optional<Recording> hdr = load_recording(one, err);
            if (!hdr) {
                malformed_++;
                st_.last_err = "refused session header: " + err;
                return;
            }
            cur_ = *hdr;
            // load_recording saw no `end`, so it marked the recording torn.
            // It is not torn — it has not STARTED yet.
            cur_.torn = false;
            cur_.has_end = false;
        }
        open_ = true;
        return;
    }

    if (!j.contains("k") || !j["k"].is_string()) {
        malformed_++;
        return;
    }
    const std::string k = j["k"].get<std::string>();

    // ---- lifecycle: the three serve-only kinds -----------------------------
    if (k == "session" || k == "cmd" || k == "err") {
        notes_.push_back(LiveNote{k, j});
        if (k == "err") {
            st_.last_err = j.value("reason", "(no reason given)");
            return; // an err never ends a session
        }
        if (k != "session")
            return;
        const std::string state = j.value("state", "");
        if (state == "started") {
            st_.state = LiveState::Running;
            st_.mode = j.value("mode", "");
            st_.pid = j.value("pid", 0L);
            st_.sessions_started++;
            st_.skip_code = 0;
            st_.skip_reason.clear();
            st_.last_stop_reason.clear();
            st_.paused_dropped = 0;
        } else if (state == "stopped" || state == "skip") {
            if (state == "skip") {
                // A skip is a SUCCESS with nothing to report. Keeping it in a
                // field of its own is what stops the UI rendering it as an
                // error, which is the single most common way this fact gets
                // misreported.
                const json &s = j.contains("skip") ? j["skip"] : json::object();
                st_.skip_code = s.is_object() ? s.value("code", 0) : 0;
                st_.skip_reason = s.is_object() ? s.value("reason", "") : "";
            }
            st_.last_stop_reason = j.value("reason", state);
            st_.paused_dropped = as_u64(
                j.contains("paused_dropped") ? j["paused_dropped"] : json(0),
                0);
            st_.sessions_ended++;
            st_.mode.clear();
            st_.state = LiveState::Idle; // the jack is free again
            // The terminal event follows the `end` footer, so a well-behaved
            // session is already closed here; this catches one that was not.
            close_current(/*torn=*/!cur_.has_end);
        }
        return;
    }

    // ---- ordinary recording events ----------------------------------------
    if (!open_) {
        // An event with no header cannot be attributed to any provenance, and
        // provenance is mandatory — so it is counted, never silently adopted.
        malformed_++;
        return;
    }
    if (k == "end") {
        cur_.has_end = true;
        if (j.contains("events"))
            cur_.declared_events = as_u64(j["events"], 0);
        if (j.contains("truncated") && j["truncated"].is_boolean())
            cur_.end_truncated = j["truncated"].get<bool>();
        if (j.contains("drops") && j["drops"].is_object()) {
            const json &d = j["drops"];
            if (d.contains("lost"))
                cur_.drops_lost = as_u64(d["lost"], 0);
            if (d.contains("throttled") && d["throttled"].is_boolean())
                cur_.drops_throttled = d["throttled"].get<bool>();
        }
        close_current(/*torn=*/false);
        return;
    }
    // The stream position, counted the same way the file loader counts it — a
    // live session and a replayed file must agree about event ORDER, which is
    // what the region view's invocation split reads (08-observer-views.md T6).
    cur_.by_kind[k].push_back(Event{k, j, cur_.next_seq++});
    if (!is_known_kind(k))
        cur_.unknown_kinds++;
}

} // namespace asmdesk
