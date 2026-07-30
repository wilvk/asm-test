// test_converge.cpp — the cross-thread convergence detector and the incremental
// live feed (docs/internal/gui/10-spacetime-3d-overview.md T5). Null harness, no
// display. Two halves, mirroring test_live_session's split:
//
//   1. PURE detection over hand-built per-tid trajectories (no subprocess): two
//      trajectories that touch a shared cell produce EXACTLY ONE mark; disjoint
//      ones produce NONE; the window bounds it; and a relative-basis / statistical
//      path never converges (it is labelled, not placed on the address plane).
//   2. The INCREMENTAL FEED, driven against desktop/test/fixtures/fake_serve_threads.sh:
//      a LiveSession streams a two-thread abs trace, and T3's builder re-run on the
//      GROWING recording yields per-tid trajectories that GROW — asserted against a
//      fake serve host, so it runs on any machine. The desktop consumes the session
//      the app already owns (D6/D9); it opens no ptrace of its own — this binary
//      links session.o (the SUBPROCESS host) + the pure space/ models and NOTHING
//      else (no ImGui, no GL, no engine), which is that guarantee made mechanical.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <time.h>

#include "doc/recording.h"
#include "live/session.h"
#include "space/converge.h"
#include "space/projection.h"
#include "space/trajectory.h"

#ifndef ASMTEST_FIXTURE_DIR
#error "ASMTEST_FIXTURE_DIR must be defined by the build (mk/desktop.mk)"
#endif

using namespace asmdesk;
using namespace asmdesk::space;

static int failures;

static void fail(const std::string &what, const std::string &why) {
    std::fprintf(stderr, "FAIL %s: %s\n", what.c_str(), why.c_str());
    failures++;
}
static void check(const std::string &what, bool cond, const std::string &why) {
    if (!cond)
        fail(what, why);
}

// A projection over one code region wide enough to give every test address its own
// cell (len is a power of four -> a 1:1 domain, so distinct addresses land in
// distinct cells and only a genuinely shared address shares a cell).
static Projection region_proj(uint64_t base, uint64_t len) {
    Region r;
    r.base = base;
    r.len = len;
    r.kind = Region::Code;
    std::vector<Region> v{r};
    return build_projection(std::move(v));
}

// Build one exact per-tid trajectory from (t, addr) pairs.
static Trajectory traj(int32_t tid, uint32_t flags,
                       const std::vector<std::pair<uint64_t, uint64_t>> &pts) {
    Trajectory tr;
    tr.tid = tid;
    tr.flags = flags;
    for (const auto &ta : pts) {
        TrajPoint p;
        p.t = ta.first;
        p.addr = ta.second;
        p.fidelity = TrajPoint::Exact;
        p.is_access = false;
        p.tid = tid;
        tr.points.push_back(p);
    }
    return tr;
}

static size_t total_pc(const TrajectorySet &ts) {
    size_t n = 0;
    for (const Trajectory &tr : ts.trajectories)
        for (const TrajPoint &p : tr.points)
            if (!p.is_access)
                n++;
    return n;
}

static size_t pc_of_tid(const TrajectorySet &ts, int32_t tid) {
    size_t n = 0;
    for (const Trajectory &tr : ts.trajectories)
        if (tr.tid == tid)
            for (const TrajPoint &p : tr.points)
                if (!p.is_access)
                    n++;
    return n;
}

// ---------------------------------------------------------------------------
// 1. pure detection
// ---------------------------------------------------------------------------
static void test_detection() {
    Projection proj = region_proj(0x400000, 0x2000);

    // === one shared cell -> EXACTLY ONE mark ================================
    {
        TrajectorySet ts;
        ts.trajectories.push_back(traj(
            1, 0,
            {{0, 0x400000}, {1, 0x400100}, {2, 0x400800}})); // 0x400800 shared
        ts.trajectories.push_back(traj(
            2, 0,
            {{0, 0x400200}, {1, 0x400800}, {2, 0x400400}})); // 0x400800 shared
        ConvergenceSet cs = detect_convergences(ts, proj);
        check("one-mark: exactly one convergence", cs.marks.size() == 1,
              "got " + std::to_string(cs.marks.size()));
        if (cs.marks.size() == 1) {
            const ConvergenceMark &m = cs.marks[0];
            check("one-mark: tids canonical (a<b)",
                  m.tid_a == 1 && m.tid_b == 2,
                  "tids " + std::to_string(m.tid_a) + "," +
                      std::to_string(m.tid_b));
            check("one-mark: both endpoints are the shared address",
                  m.addr_a == 0x400800 && m.addr_b == 0x400800, "wrong addrs");
            check("one-mark: gap is |2-1| = 1", m.gap == 1,
                  "gap " + std::to_string(m.gap));
            check("one-mark: the two vertices carry their own steps",
                  m.t_a == 2 && m.t_b == 1, "steps wrong");
            // The mark's cell is the shared address's plane cell.
            float u = 0, v = 0;
            proj.project(0x400800, &u, &v);
            uint32_t n = 1u << proj.order;
            uint32_t x = static_cast<uint32_t>(u * n),
                     y = static_cast<uint32_t>(v * n);
            check("one-mark: mark cell is the shared cell", m.cell == y * n + x,
                  "cell mismatch");
        }
        // It is a HINT — labelled so, in the data.
        check("one-mark: the set is labelled a hint", ConvergenceSet::kHint,
              "kHint is false");
        ConvergenceSet cs2 = cs;
        check("one-mark: the hint label is non-empty",
              cs2.label() && *cs2.label(), "empty label");
    }

    // === two threads that DWELL in one cell still make ONE hint =============
    // Thread 1 sits in the shared cell for several steps; thread 2 passes once.
    // The detector keeps the closest crossing, not one mark per touch.
    {
        TrajectorySet ts;
        ts.trajectories.push_back(
            traj(1, 0,
                 {{0, 0x400800}, {1, 0x400800}, {2, 0x400800}, {3, 0x400800}}));
        ts.trajectories.push_back(traj(2, 0, {{2, 0x400800}}));
        ConvergenceSet cs = detect_convergences(ts, proj);
        check("dwell: one hint for many touches", cs.marks.size() == 1,
              "got " + std::to_string(cs.marks.size()));
        if (cs.marks.size() == 1)
            check("dwell: the closest crossing wins (gap 0)",
                  cs.marks[0].gap == 0,
                  "gap " + std::to_string(cs.marks[0].gap));
    }

    // === disjoint trajectories -> NO marks ==================================
    {
        TrajectorySet ts;
        ts.trajectories.push_back(
            traj(1, 0, {{0, 0x400000}, {1, 0x400100}, {2, 0x400200}}));
        ts.trajectories.push_back(
            traj(2, 0, {{0, 0x400400}, {1, 0x400500}, {2, 0x400600}}));
        ConvergenceSet cs = detect_convergences(ts, proj);
        check("no-mark: disjoint threads never converge", cs.marks.empty(),
              "got " + std::to_string(cs.marks.size()));
    }

    // === the window bounds it ===============================================
    // Same shared cell, but the two touches are 100 steps apart: outside the
    // default window (64) there is no hint; widen the window and it appears.
    {
        TrajectorySet ts;
        ts.trajectories.push_back(traj(1, 0, {{0, 0x400000}, {100, 0x400800}}));
        ts.trajectories.push_back(traj(2, 0, {{0, 0x400800}}));
        check("window: a far-apart crossing is not a hint (default 64)",
              detect_convergences(ts, proj).marks.empty(), "got a mark");
        ConvergeParams wide;
        wide.window = 200;
        check("window: widening the window admits it",
              detect_convergences(ts, proj, wide).marks.size() == 1,
              "wide window found none");
    }

    // === same-thread self-crossing is NOT a convergence =====================
    {
        TrajectorySet ts;
        ts.trajectories.push_back(
            traj(1, 0, {{0, 0x400800}, {1, 0x400800}})); // one thread, twice
        check("self: one thread revisiting a cell is not a convergence",
              detect_convergences(ts, proj).marks.empty(), "got a mark");
    }

    // === relative-basis / statistical paths never converge (fidelity) ========
    // Both endpoints project and share a cell, but the paths are LABELLED, not
    // placed on the address plane — a convergence over them would be a false
    // address-space claim, so the detector skips them by flag, not by luck.
    {
        TrajectorySet rel;
        rel.trajectories.push_back(
            traj(1, TRAJ_RELATIVE_BASIS, {{0, 0x400800}}));
        rel.trajectories.push_back(
            traj(2, TRAJ_RELATIVE_BASIS, {{0, 0x400800}}));
        check("fidelity: two relative-basis paths do not converge",
              detect_convergences(rel, proj).marks.empty(),
              "a rel path was placed on the plane");

        TrajectorySet stat;
        stat.trajectories.push_back(traj(1, 0, {{0, 0x400800}}));
        stat.trajectories.push_back(traj(2, TRAJ_STATISTICAL, {{0, 0x400800}}));
        check("fidelity: a statistical layer never joins a convergence",
              detect_convergences(stat, proj).marks.empty(),
              "a statistical residency was treated as a thread path");
    }

    // === an empty projection places nothing, gracefully =====================
    {
        Projection empty = build_projection({});
        TrajectorySet ts;
        ts.trajectories.push_back(traj(1, 0, {{0, 0x400800}}));
        ts.trajectories.push_back(traj(2, 0, {{0, 0x400800}}));
        check("empty projection yields no marks (no crash)",
              detect_convergences(ts, empty).marks.empty(), "got a mark");
    }
}

// ---------------------------------------------------------------------------
// 1b. 36 T5: convergence admits an ANCHORED rel path, and only an anchored one
// ---------------------------------------------------------------------------
static void test_anchored_convergence() {
    const uint64_t base = 0x400000;
    Projection proj = region_proj(base, 0x2000);
    const uint32_t A = TRAJ_RELATIVE_BASIS | TRAJ_ANCHORED;

    // Two ANCHORED rel paths (base+off) sharing a cell converge — 36 T5 admits
    // them where an unanchored rel path is still refused. The mark carries the
    // ABSOLUTE address, not the raw offset.
    {
        TrajectorySet ts;
        ts.trajectories.push_back(
            traj(1, A, {{0, base + 0x0}, {1, base + 0x800}}));
        ts.trajectories.push_back(
            traj(2, A, {{0, base + 0x800}, {1, base + 0x400}}));
        ConvergenceSet cs = detect_convergences(ts, proj);
        check("anchored: two anchored rel paths converge", cs.marks.size() == 1,
              "got " + std::to_string(cs.marks.size()));
        if (cs.marks.size() == 1)
            check("anchored: the mark carries the ABSOLUTE address (base+off)",
                  cs.marks[0].addr_a == base + 0x800 &&
                      cs.marks[0].addr_b == base + 0x800,
                  "not the absolute address");
    }

    // S1: an anchored path and an equivalent ABS path yield IDENTICAL sets — the
    // anchored base+off projects exactly as a measured absolute PC would.
    {
        const std::vector<std::pair<uint64_t, uint64_t>> p1 = {
            {0, base + 0x100}, {1, base + 0x800}};
        const std::vector<std::pair<uint64_t, uint64_t>> p2 = {
            {0, base + 0x800}, {1, base + 0x200}};
        TrajectorySet anc, abs;
        anc.trajectories.push_back(traj(1, A, p1));
        anc.trajectories.push_back(traj(2, A, p2));
        abs.trajectories.push_back(traj(1, 0, p1));
        abs.trajectories.push_back(traj(2, 0, p2));
        ConvergenceSet ca = detect_convergences(anc, proj);
        ConvergenceSet cb = detect_convergences(abs, proj);
        check("anchored: identical to the equivalent abs ConvergenceSet",
              ca.marks.size() == 1 && cb.marks.size() == 1 &&
                  ca.marks[0].cell == cb.marks[0].cell &&
                  ca.marks[0].addr_a == cb.marks[0].addr_a &&
                  ca.marks[0].addr_b == cb.marks[0].addr_b &&
                  ca.marks[0].gap == cb.marks[0].gap,
              "an anchored path diverged from its abs equivalent");
    }

    // An anchored path and an UNANCHORED rel path do NOT converge — the unanchored
    // one is excluded, so there is no cross-thread pair (the canary that fails if
    // someone "fixes" the mask by deleting the rel bit outright).
    {
        TrajectorySet ts;
        ts.trajectories.push_back(traj(1, A, {{0, base + 0x800}}));
        ts.trajectories.push_back(
            traj(2, TRAJ_RELATIVE_BASIS, {{0, base + 0x800}}));
        check("anchored: an anchored path never joins an unanchored one",
              detect_convergences(ts, proj).marks.empty(),
              "an unanchored rel path leaked into a convergence");
    }

    // The clamp case (S2): a shared cell reached ONLY by placed==false vertices
    // yields zero marks. The unplaced vertices' addr even PROJECTS here, so only
    // the !placed guard prevents a spurious mark — deleting it fails this.
    {
        auto pt = [](uint64_t t, uint64_t addr, bool placed) {
            TrajPoint p;
            p.t = t;
            p.addr = addr;
            p.tid = 0;
            p.is_access = false;
            p.fidelity = TrajPoint::Exact;
            p.placed = placed;
            return p;
        };
        TrajectorySet ts;
        Trajectory a;
        a.tid = 1;
        a.flags = A;
        Trajectory b;
        b.tid = 2;
        b.flags = A;
        a.points.push_back(pt(0, base + 0x100, true));  // distinct placed cell
        a.points.push_back(pt(1, base + 0x800, false)); // shared, but UNPLACED
        b.points.push_back(pt(0, base + 0x200, true));  // distinct placed cell
        b.points.push_back(pt(1, base + 0x800, false)); // shared, but UNPLACED
        ts.trajectories.push_back(a);
        ts.trajectories.push_back(b);
        check(
            "anchored: a cell reached only by unplaced vertices yields no mark",
            detect_convergences(ts, proj).marks.empty(),
            "an unplaced (clamped) vertex was bucketed");
    }

    // End-to-end: a rel `trace` with two tids sharing an offset + one codeimage.
    // The BUILDER anchors both paths (base+off) and the detector marks them —
    // proving the predicate matches what build_trajectories actually emits.
    {
        std::string nd =
            "{\"asmtrace\":1,\"provenance\":{\"backend\":\"emu-l0\","
            "\"exact\":true,\"trust\":\"exact\"},\"arch\":\"x86_64\"}\n";
        nd += "{\"k\":\"codeimage\",\"base\":4194304,\"len\":8192,"
              "\"version\":0}\n";
        nd += "{\"k\":\"trace\",\"basis\":\"rel\",\"off\":256,\"tid\":7}\n";
        nd +=
            "{\"k\":\"trace\",\"basis\":\"rel\",\"off\":2048,\"tid\":7}\n"; // sh
        nd +=
            "{\"k\":\"trace\",\"basis\":\"rel\",\"off\":2048,\"tid\":9}\n"; // sh
        nd += "{\"k\":\"trace\",\"basis\":\"rel\",\"off\":512,\"tid\":9}\n";
        nd += "{\"k\":\"end\",\"events\":6,\"truncated\":false}\n";
        std::istringstream in(nd);
        std::string err;
        auto rec = load_recording(in, err);
        check("e2e: fixture loads", static_cast<bool>(rec), err);
        if (rec) {
            Projection p = region_proj(base, 0x2000);
            TrajectorySet ts = build_trajectories(*rec, p);
            check("e2e: the builder anchored the rel trace", ts.anchored,
                  "rel trace not anchored");
            ConvergenceSet cs = detect_convergences(ts, p);
            check("e2e: one convergence on the shared anchored cell",
                  cs.marks.size() == 1,
                  "got " + std::to_string(cs.marks.size()));
            if (cs.marks.size() == 1)
                check("e2e: the mark is the ABSOLUTE shared address",
                      cs.marks[0].addr_a == base + 0x800 &&
                          cs.marks[0].addr_b == base + 0x800,
                      "not the absolute shared address");
        }
    }
}

// ---------------------------------------------------------------------------
// 2. the incremental live feed, against the fake two-thread serve host
// ---------------------------------------------------------------------------
static void nap_ms(int ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000 * 1000};
    nanosleep(&ts, nullptr);
}

static void test_live_feed() {
    const std::string fake =
        std::string(ASMTEST_FIXTURE_DIR) + "/fake_serve_threads.sh";

    LiveSession s;
    LiveSession::Spec spec;
    spec.asmspy_path = fake;
    std::string err;
    if (!s.start(spec, err)) {
        fail("feed/start", "could not spawn the fake threads host: " + err);
        return;
    }
    check("feed/command",
          s.status().command.find("--serve") != std::string::npos,
          "the spawned command should carry --serve");

    s.send_start("threads", 4242);

    // --- growth milestone 1: catch the GROWING recording mid-stream ---------
    // T3's builder re-run on the growing recording is the incremental feed: as the
    // session appends trace events, the per-tid trajectories grow. Batch 1 arrives
    // before the fixture's sleep, so a poll here sees a PARTIAL trajectory.
    size_t n1 = 0;
    bool saw_growing = false;
    for (int i = 0; i < 1000 && !saw_growing; i++) {
        s.poll();
        if (s.growing()) {
            TrajectorySet g = build_trajectories(*s.growing());
            size_t tot = total_pc(g);
            if (tot >= 1) {
                n1 = tot;
                saw_growing = true;
                break;
            }
        }
        nap_ms(5);
    }
    check("feed/growing-seen", saw_growing,
          "no growing recording with a trajectory was observed");
    check("feed/partial", n1 <= 5,
          "milestone 1 should be a partial trajectory (batch 1 <= 5 vertices), "
          "got " +
              std::to_string(n1));

    // --- growth milestone 2: the session completes --------------------------
    bool done = false;
    for (int i = 0; i < 2000 && !done; i++) {
        s.poll();
        if (s.recordings().size() == 1)
            done = true;
        else
            nap_ms(5);
    }
    check("feed/completed", done, "the threads session never completed");
    if (!done) {
        s.shutdown();
        return;
    }

    TrajectorySet full = build_trajectories(s.recordings()[0]);
    size_t n2 = total_pc(full);
    check("feed/grew", n2 > n1,
          "the trajectory did not grow: n1=" + std::to_string(n1) +
              " n2=" + std::to_string(n2));
    check("feed/final-count", n2 == 9,
          "the completed stream has 9 PC vertices, got " + std::to_string(n2));

    // --- per-tid: two coloured threads, each grown to its own count ---------
    check("feed/two-tids", full.trajectories.size() == 2,
          "expected one trajectory per tid, got " +
              std::to_string(full.trajectories.size()));
    check("feed/tid7-grew", pc_of_tid(full, 7) == 5,
          "tid 7 should have 5 vertices, got " +
              std::to_string(pc_of_tid(full, 7)));
    check("feed/tid9-grew", pc_of_tid(full, 9) == 4,
          "tid 9 should have 4 vertices, got " +
              std::to_string(pc_of_tid(full, 9)));
    check("feed/abs-basis", full.basis == "abs" && !full.refused(),
          "the live abs stream should build a real address path");

    // --- the whole pipe end to end: one live convergence on the shared cell -
    // Both threads touch 0x401200; over a projection of the fixture's code region
    // that is exactly one cross-thread convergence hint.
    Projection proj = region_proj(0x401000, 0x1000);
    ConvergenceSet cs = detect_convergences(full, proj);
    check("feed/one-convergence", cs.marks.size() == 1,
          "expected exactly one live convergence, got " +
              std::to_string(cs.marks.size()));
    if (cs.marks.size() == 1) {
        const ConvergenceMark &m = cs.marks[0];
        check("feed/convergence-tids", m.tid_a == 7 && m.tid_b == 9,
              "converging tids wrong");
        check("feed/convergence-addr",
              m.addr_a == 0x401200 && m.addr_b == 0x401200,
              "converging address wrong");
    }

    s.shutdown();
    check("feed/reaped", s.status().host_exited, "the host should be reaped");
    // The overlay opened no ptrace of its own — it read only the session's
    // recordings (D6/D9). Nothing to assert but the link line, which carries it.
}

int main() {
    test_detection();
    test_anchored_convergence();
    test_live_feed();
    if (failures) {
        std::fprintf(stderr, "%d converge check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_converge: all checks passed\n");
    return 0;
}
