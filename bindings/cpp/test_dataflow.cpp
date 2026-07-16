// test_dataflow.cpp — C++ binding smoke for the data-flow tier (Phase 6 + F7).
// Mirrors the Python binding's semantics through the typed C++ wrappers, and — F7 —
// attaches to a REAL live victim process (bindings/dataflow_victim.c) by pid.
#include "asmtest_dataflow.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <vector>

static int g_n = 0;
static int g_fail = 0;

static void check(bool cond, const char* desc) {
    ++g_n;
    if (cond)
        std::printf("ok %d - %s\n", g_n, desc);
    else {
        std::printf("not ok %d - %s\n", g_n, desc);
        g_fail = 1;
    }
}

// --------------------------------------------------------------------------
// F7 — the live victim: spawn bindings/dataflow_victim, learn its region base.
//
// `a`/`b` are OURS, so the expected result is a property of THIS run — a wrapper
// that hardcodes an answer cannot satisfy two victims with different args.
// --------------------------------------------------------------------------
struct Victim {
    pid_t pid = -1;
    std::uint64_t base = 0;
    std::size_t len = 0;
    std::string counterPath;

    bool spawn(const char* exe, const char* path, long a, long b) {
        counterPath = path;
        int fds[2];
        if (pipe(fds) != 0) return false;
        pid = fork();
        if (pid < 0) return false;
        if (pid == 0) {
            close(fds[0]);
            dup2(fds[1], STDOUT_FILENO);
            close(fds[1]);
            std::string as = std::to_string(a), bs = std::to_string(b);
            execl(exe, exe, path, as.c_str(), bs.c_str(), (char*)nullptr);
            _exit(127);
        }
        close(fds[1]);
        // Read the victim's one handshake line: "base=0x<hex> len=<n>".
        std::string line;
        char c;
        while (read(fds[0], &c, 1) == 1 && c != '\n') line += c;
        close(fds[0]);
        // The victim's OWN pid (see bindings/dataflow_victim.c) — right in every
        // binding, including those whose spawn goes through a shell.
        int reported = 0;
        if (std::sscanf(line.c_str(), "base=0x%llx len=%zu pid=%d",
                        reinterpret_cast<unsigned long long*>(&base), &len,
                        &reported) != 3)
            return false;
        pid = reported;
        return true;
    }
    std::uint64_t counter() const {
        int fd = open(counterPath.c_str(), O_RDONLY);
        if (fd < 0) return 0;
        std::uint64_t v = 0;
        ssize_t got = read(fd, &v, sizeof v);
        close(fd);
        return got == (ssize_t)sizeof v ? v : 0;
    }
    ~Victim() {
        if (pid > 0) {
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
        }
    }
};

static void msleep(long ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000 * 1000};
    nanosleep(&ts, nullptr);
}

// ETRACE is NOT a skip. ptrace is a capability the lane can be GIVEN
// (--cap-add=SYS_PTRACE / seccomp=unconfined), and the victim opts in via
// PR_SET_PTRACER_ANY, so a refusal means the lane is misconfigured — be loud.
static void checkRc(int rc, const char* what) {
    if (rc == asmtest::dataflow::kPtraceEtrace)
        std::printf("# %s: ptrace refused (ETRACE) — the lane needs "
                    "--cap-add=SYS_PTRACE; this is NOT a valid skip\n", what);
    check(rc == asmtest::dataflow::kPtraceOk, what);
}

int main() {
    using namespace asmtest::dataflow;

    // --- GC-move canonicalizer (forward-to-final) --- //
    check(gcmove_canon({}, 0, 0x1234) == 0x1234, "gcmove: empty move set is identity");
    std::vector<GcMove> mv = {{0x1000, 0x2000, 0x100, 5}};
    check(gcmove_canon(mv, 3, 0x1010) == 0x2010, "gcmove: pre-move addr forwards to final");
    check(gcmove_canon(mv, 3, 0x1000) == 0x2000, "gcmove: object base forwards");
    check(gcmove_canon(mv, 3, 0x10FF) == 0x20FF, "gcmove: last byte of half-open window forwards");
    check(gcmove_canon(mv, 3, 0x1100) == 0x1100, "gcmove: one past the window does not forward");
    check(gcmove_canon(mv, 5, 0x1010) == 0x1010, "gcmove: at-move-step observation not forwarded");
    check(gcmove_canon(mv, 3, 0x3000) == 0x3000, "gcmove: out-of-range addr unchanged");
    std::vector<GcMove> mv2 = {{0x1000, 0x2000, 0x100, 3}, {0x2000, 0x3000, 0x100, 6}};
    check(gcmove_canon(mv2, 1, 0x1010) == 0x3010, "gcmove: two compactions compose to final");

    // --- method resolver (tiered re-JIT aware) --- //
    std::vector<Method> ms = {
        {0x1000, 0x40, "Foo", 3}, {0x2000, 0x20, "Bar", 1}, {0x3000, 0, "Baz", 2}};
    check(method_resolve_pc(ms, 0x1000) == 0, "method: Foo range start");
    check(method_resolve_pc(ms, 0x103F) == 0, "method: Foo last byte (half-open)");
    check(method_resolve_pc(ms, 0x1040) == -1, "method: one past Foo -> none");
    check(method_resolve_pc(ms, 0x2010) == 1, "method: Bar range");
    check(method_resolve_pc(ms, 0x3000) == 2, "method: Baz point match");
    check(method_resolve_pc(ms, 0x3001) == -1, "method: Baz is point-only");
    std::vector<Method> rj = {{0x1000, 0x40, "Foo", 1}, {0x1000, 0x40, "Foo", 5}};
    check(method_resolve_pc(rj, 0x1010) == 1, "method: tiered re-JIT newest version wins");
    check(method_resolve_pc({}, 0x1000) == -1, "method: empty map -> -1");

    // --- L0->L1->L2 pipeline (ValueTrace: build -> def-use -> slice) --- //
    {
        ValueTrace vt;
        vt.step(0x00, {}, {Reg(10)});                 // def r10
        vt.step(0x03, {Reg(10)}, {Reg(11)});          // r11 <- r10
        vt.step(0x06, {Reg(11)}, {Reg(12)});          // r12 <- r11
        check(vt.forwardSlice(0) == std::set<std::uint32_t>({0, 1, 2}),
              "pipeline: register move chain forward slice");
        check(vt.backwardSlice(2) == std::set<std::uint32_t>({0, 1, 2}),
              "pipeline: register move chain backward slice");
        check(vt.forwardSlice(2) == std::set<std::uint32_t>({2}),
              "pipeline: nothing downstream of the tail");
    }
    {
        ValueTrace vt;  // load-after-store through memory
        vt.step(0x00, {Reg(8)}, {MemAbs(0x7FFF0000)});
        vt.step(0x04, {MemAbs(0x7FFF0000)}, {Reg(9)});
        check(vt.forwardSlice(0) == std::set<std::uint32_t>({0, 1}),
              "pipeline: load-after-store def-use edge through memory");
    }
    {
        ValueTrace vt;  // independent chains must not cross-link
        vt.step(0x00, {}, {Reg(1)});
        vt.step(0x02, {Reg(1)}, {Reg(2)});
        vt.step(0x04, {}, {Reg(3)});
        vt.step(0x06, {Reg(3)}, {Reg(4)});
        check(vt.forwardSlice(0) == std::set<std::uint32_t>({0, 1}),
              "pipeline: no spurious cross-link between independent chains");
    }

    // ------------------------------------------------------------------
    // F7 — live-attach data flow over a REAL attached pid.
    //
    // Every assertion is POSITIVE and keyed to something only a working capture
    // produces (the region's return value, the exact step count, the def-use
    // shape). Nothing here is guarded by "if we captured anything" — that shape is
    // what let a sibling binding's lane pass while blind, because an empty capture
    // IS the failure signature.
    // ------------------------------------------------------------------
#if defined(__linux__) && defined(__x86_64__)
    {
        const char* exe = std::getenv("ASMTEST_DATAFLOW_VICTIM");
        if (!exe) {
            // The lane always exports this; missing means a misconfigured lane, and
            // silently skipping every live test is the hole this suite must not have.
            std::printf("Bail out! ASMTEST_DATAFLOW_VICTIM unset; run `make "
                        "dataflow-cpp-test`\n");
            return 1;
        }
        // Probed, not a symbol-resolves check: EINVAL (real) vs ENOSYS (stub).
        check(live_attach_available(),
              "live: tier is real on linux/x86_64 (EINVAL, not ENOSYS)");

        {
            Victim vic;
            check(vic.spawn(exe, "/tmp/asmtest-df-cpp-1.counter", 7, 5),
                  "live: spawned a victim and read its region base");
            ValueTrace vt(64, 512);
            AttachResult r = vt.attachPid(vic.pid, vic.base, vic.len);
            checkRc(r.rc, "live: attach_pid a FOREIGN running pid + stepped the region");
            // The region really executed IN the victim: rax = rdi + rsi.
            check(r.result == 12, "live: attach_pid region returned 12 (rax = rdi + rsi)");
            // Exactly df_chain's six in-region instructions — not "some".
            check(vt.steps() == 6, "live: six in-region steps captured over the victim");
            check(vt.recs() > 0, "live: operand records captured");

            // SURVIVAL: we attached to a process we do not own; it must outlive the
            // detach. The counter is the victim's own writes, so movement proves it.
            std::uint64_t c0 = vic.counter();
            msleep(50);
            check(vic.counter() > c0, "live: victim SURVIVED the detach (counter advanced)");

            // The def-use over the LIVE trace has the real shape: the store at step 1
            // feeds the load at step 2 (a MEMORY edge through [rsp-8]), chaining on.
            check(vt.backwardSlice(4) == std::set<std::uint32_t>({0, 1, 2, 3, 4}),
                  "live: backward slice(step4) = {0,1,2,3,4} over the live capture");
            std::set<std::uint32_t> fwd = vt.forwardSlice(0);
            check(fwd.count(4) == 1, "live: forward slice(step0) reaches the final mov");
            // Negative control on the SHAPE: the `ret` consumes none of the chain, so
            // reaching it would mean the graph links everything to everything.
            check(fwd.count(5) == 0, "live: forward slice(step0) excludes the ret");
        }
        {
            // THE anti-hardcode control: a second victim, different args, same wrapper.
            Victim vic;
            check(vic.spawn(exe, "/tmp/asmtest-df-cpp-2.counter", 17, 25),
                  "live: spawned a second victim with different args");
            ValueTrace vt(64, 512);
            AttachResult r = vt.attachPid(vic.pid, vic.base, vic.len);
            checkRc(r.rc, "live: attach_pid the second victim");
            check(r.result == 42, "live: result TRACKS the victim's args (17+25=42)");
            check(vt.steps() == 6, "live: six steps on the second victim too");
        }
        {
            Victim vic;
            check(vic.spawn(exe, "/tmp/asmtest-df-cpp-3.counter", 9, 4),
                  "live: spawned a victim for the worker-targeting entry");
            ValueTrace vt(64, 512);
            // onlyTid 0: step whichever thread enters the region (here, the only one).
            AttachResult r = vt.attachPidTid(vic.pid, 0, vic.base, vic.len);
            checkRc(r.rc, "live: attach_pid_tid stepped the entering thread");
            check(r.result == 13, "live: attach_pid_tid region returned 13 (9+4)");
            check(vt.steps() == 6, "live: attach_pid_tid captured six steps");
        }
        {
            Victim vic;
            check(vic.spawn(exe, "/tmp/asmtest-df-cpp-4.counter", 20, 3),
                  "live: spawned a victim for the JIT-aware entry");
            ValueTrace vt(64, 512);
            AttachResult r = vt.attachJit(vic.pid, 0, vic.base, vic.len);
            checkRc(r.rc, "live: attach_jit stepped the region");
            check(r.result == 23, "live: attach_jit region returned 23 (20+3)");
            check(vt.steps() == 6, "live: attach_jit captured six steps");
            // The producer's OWN survival report — the house rule that a foreign
            // target is never killed, asserted from its side.
            check(r.survived == 1, "live: attach_jit reported the target as survived");
            std::uint64_t c0 = vic.counter();
            msleep(50);
            check(vic.counter() > c0, "live: attach_jit victim kept running after detach");
        }
        {
            // Negative control: the wrapper must surface the producer's rejections
            // rather than manufacture success.
            ValueTrace vt(8, 8);
            check(vt.attachPid(12345, 0x1000, 0).rc == kPtraceEinval,
                  "live: zero-length region is rejected (EINVAL)");
            check(vt.attachPid(0, 0x1000, 21).rc == kPtraceEinval,
                  "live: pid 0 is rejected (EINVAL)");
            check(vt.attachPid(0x7FFFFFF0, 0x1000, 21).rc != kPtraceOk,
                  "live: attaching to a nonexistent pid never returns OK");
        }
    }
#else
    // Genuinely off-tier ISA: src/dataflow_ptrace.c is Linux x86-64 only.
    std::printf("# SKIP live-attach: not linux/x86-64\n");
#endif

    std::printf("1..%d\n", g_n);
    return g_fail;
}
