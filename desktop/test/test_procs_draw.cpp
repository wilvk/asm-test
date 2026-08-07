// test_procs_draw.cpp — the Processes pane draws its LINEAGE, over a null
// backend and a hand-built /proc snapshot.
//
// The pure half (proc_tree_layout: depth, order, the four ProcParent cases,
// the cycle cut) is pinned in test_inspect.cpp against no ImGui at all. What
// that cannot see is whether any of it reaches the screen — a layout can be
// perfectly correct while the pane draws a flat table, names no parent, or
// silently swallows the "hidden by the filter" line that is the only thing
// explaining an indented row with nothing above it. So every check here reads
// ImGui's own log of what it RENDERED (LogToClipboard walks the widget tree
// emitting literal text; the null backend's clipboard is a plain in-process
// buffer) and asserts on that text, never on the absence of a crash.
//
// The pane reads /proc on its own (inspect_scan) unless `scanned` is already
// set, so every scenario below pre-loads InspectState::rows with a snapshot it
// controls. That is the only way to test a lineage at all: the shapes that
// matter — a filtered-away parent, a ppid naming a process that has since
// exited, a cycle — are precisely the ones a live /proc will not produce on
// demand.
//
// Each check was verified BY MUTATION (see the notes at each site): reverting
// the behaviour under test flips this binary to FAIL.
#include <cstdio>
#include <string>
#include <vector>

#include "imgui.h"

#include "live/inspect.h"
#include "ui/doors.h"

using namespace asmdesk;

static int failures;
static void check(const char *what, bool cond, const std::string &why) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s: %s\n", what, why.c_str());
        failures++;
    }
}
static void want(const char *what, const std::string &hay, const char *needle) {
    check(what, hay.find(needle) != std::string::npos,
          std::string("expected to find '") + needle + "' in:\n" + hay);
}
static void avoid(const char *what, const std::string &hay,
                  const char *needle) {
    check(what, hay.find(needle) == std::string::npos,
          std::string("did NOT expect to find '") + needle + "' in:\n" + hay);
}
// "a comes before b" — the one assertion a flat table cannot satisfy by
// accident once the rows are deliberately NOT in pid order.
static void want_before(const char *what, const std::string &hay,
                        const char *a, const char *b) {
    const size_t ia = hay.find(a), ib = hay.find(b);
    check(what, ia != std::string::npos && ib != std::string::npos && ia < ib,
          std::string("expected '") + a + "' before '" + b + "' in:\n" + hay);
}

// The connectors the pane draws, as UTF-8. Kept as named constants so a check
// reads as "the last child got the └", not as an escape sequence. Both live in
// the Box Drawing block the font atlas already bakes (ui/fonts.cpp).
static const char *kTee = "\xe2\x94\x9c";  // ├ — a child with siblings below
static const char *kElbow = "\xe2\x94\x94"; // └ — the last DRAWN child

// pid / ppid / comm, plus the verdict the pane's "only attachable" gate reads.
// Attach::Yes by default so the gate hides nothing unless a test asks it to —
// the lineage, not the gate, is what is under test here.
static ProcRow pr(long pid, long ppid, const char *comm,
                  Attach v = Attach::Yes) {
    ProcRow r;
    r.pid = pid;
    r.ppid = ppid;
    r.comm = comm;
    r.verdict.verdict = v;
    r.verdict.why = "same user, and ptrace_scope=0";
    return r;
}

// systemd(1) -> gnome-shell(1130) -> firefox(2201) -> two content children,
// with NetworkManager(842) as pid 1's other child. pid-sorted, as
// list_processes() leaves it.
static std::vector<ProcRow> snapshot() {
    return {
        pr(1, 0, "systemd"),
        pr(842, 1, "NetworkManager"),
        pr(1130, 1, "gnome-shell"),
        pr(2201, 1130, "firefox"),
        pr(2260, 2201, "Web Content"),
        pr(2281, 2201, "RDD Process"),
    };
}

// An InspectState with the snapshot already loaded, so draw_processes_pane
// never touches the host's real /proc.
static void load(InspectState &s, std::vector<ProcRow> rows) {
    s.rows = std::move(rows);
    s.scanned = true;
}

// Draw the pane once into a fresh frame and return everything ImGui rendered.
// The window is a fixed, modest size — the docked right rail this pane really
// lives in — rather than an auto-resizing infinite canvas.
static std::string render(InspectState &s, ImGuiIO &io) {
    io.DisplaySize = ImVec2(1280, 900);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImGui::SetNextWindowSize(ImVec2(560, 700), ImGuiCond_Always);
    ImGui::Begin("procs-under-test");
    ImGui::LogToClipboard();
    draw_processes_pane(s);
    ImGui::LogFinish();
    ImGui::End();
    ImGui::Render();
    if (ImGui::GetDrawData() == nullptr) {
        std::fprintf(stderr, "test_procs_draw: FAIL: null draw data\n");
        failures++;
    }
    const char *clip = ImGui::GetClipboardText();
    return clip ? clip : "";
}

// The pane's own sort state is read from ImGui's table sort specs, which only
// exist once the table has been submitted — so the FIRST frame establishes
// them and the second is the one worth reading. Two frames is also what a real
// session always has: nothing here is a one-frame-only state.
static std::string render2(InspectState &s, ImGuiIO &io) {
    render(s, io);
    return render(s, io);
}

int main() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    unsigned char *px = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);

    // --- 1. the tree reaches the screen ---------------------------------
    // Nesting is not a claim about the model, it is a claim about pixels: the
    // connectors must actually be submitted, and the deepest rows must carry
    // more indent than the shallow ones. Mutation: forcing use_tree false
    // drops every connector and fails the first two checks here.
    {
        InspectState s;
        load(s, snapshot());
        const std::string out = render2(s, io);
        want("tree/connector-drawn", out, kElbow);
        want("tree/tee-drawn", out, kTee);
        // Depth 3 is two levels of padding (2 spaces each) before the
        // connector. A flat table, or one that capped depth at 1, renders no
        // such run.
        want("tree/depth-indents", out, "    \xe2\x94\x9c 2260");
        want("tree/depth-indents-last", out, "    \xe2\x94\x94 2281");
        // The DEFINING property of the tree: 2201 (firefox) is drawn before
        // its children, and 842 (NetworkManager) — a LOWER pid than either —
        // comes before both, because it is pid 1's child and they are not.
        want_before("tree/parent-before-children", out, "2201", "2260");
        // The parent column names the parent whether or not the tree drew it.
        want("tree/parent-column", out, "1130 gnome-shell");
        want("tree/parent-column-root", out, "2201 firefox");
    }

    // --- 2. an unnested table still names every parent -------------------
    // The reason the parent column exists at all: it is the fact that must
    // survive the operator turning the tree off. Mutation: dropping the
    // column leaves "1130 gnome-shell" nowhere in the render.
    {
        InspectState s;
        load(s, snapshot());
        s.proc_tree = false;
        const std::string out = render2(s, io);
        avoid("flat/no-connectors", out, kElbow);
        avoid("flat/no-tees", out, kTee);
        want("flat/parent-still-named", out, "1130 gnome-shell");
        want("flat/parent-still-named-2", out, "2201 firefox");
    }

    // --- 3. a root's parent cell is an ABSENCE, not a zero ---------------
    // pid 1 has no parent in this snapshot. "0" there would be a pid, and a
    // pid that does not exist; the em dash is the same absence-vs-zero rule
    // the activity column already keeps. Mutation: printing r.ppid
    // unconditionally renders "0" and fails this.
    {
        InspectState s;
        load(s, snapshot());
        const std::string out = render2(s, io);
        // Anchored on the CELL, between its own pipes: a bare "—" needle would
        // be satisfied by the activity column two cells over, and a bare "0"
        // by any pid containing one. (Both weaker forms were tried first and
        // survived the mutation that prints r.ppid unconditionally.)
        want("root/absence-dash", out, "| systemd | — |");
        avoid("root/no-zero-ppid", out, "| systemd | 0 |");
    }

    // --- 4. THE CASE THE WHOLE DESIGN TURNS ON --------------------------
    // Filter to "Content" and the two content processes keep their real depth
    // with every ancestor gone. Without the explanatory line, that renders as
    // two rows indented under nothing — indistinguishable from a broken tree.
    // Mutation: dropping the ProcParent::Hidden arm's TextDisabled leaves the
    // indent unexplained and fails the second check.
    {
        InspectState s;
        load(s, snapshot());
        std::snprintf(s.proc_filter.buf, sizeof s.proc_filter.buf, "%s",
                      "Web Content");
        const std::string out = render2(s, io);
        want("hidden-parent/child-still-drawn", out, "2260");
        avoid("hidden-parent/ancestors-gone", out, "gnome-shell\n");
        // Its true depth is kept, not re-rooted to the left margin.
        want("hidden-parent/keeps-depth", out, "    \xe2\x94\x94 2260");
        want("hidden-parent/says-why", out,
             "(hidden by the filter / attachable gate)");
        // And it still NAMES the parent, which is what makes the gap
        // actionable rather than merely admitted.
        want("hidden-parent/still-named", out, "2201 firefox");
    }

    // --- 5. a ppid naming no row is a DIFFERENT absence ------------------
    // The parent exited between the readdir and the read, or lives outside our
    // pid namespace. No filter change brings it back, so it must not read as
    // "hidden". Mutation: collapsing Unknown into Hidden shows the wrong
    // remedy and fails the second check here.
    {
        InspectState s;
        load(s, {pr(1, 0, "systemd"), pr(3000, 2999, "orphan")});
        const std::string out = render2(s, io);
        want("orphan/says-not-in-snapshot", out, "(not in this snapshot)");
        avoid("orphan/not-called-hidden", out,
              "(hidden by the filter / attachable gate)");
        // The ppid is still worth showing — it is a real number we read.
        want("orphan/ppid-shown", out, "2999");
    }

    // --- 6. the "only attachable" gate is the OTHER way a parent vanishes -
    // Same rule as the filter, reached through the gate instead: an unhidden
    // child of a hidden parent keeps its depth and says why.
    {
        InspectState s;
        std::vector<ProcRow> rows = snapshot();
        rows[3].verdict.verdict = Attach::No; // firefox itself: not attachable
        rows[3].verdict.why = "the target runs as a different user";
        load(s, std::move(rows));
        s.hide_unattachable = true;
        const std::string out = render2(s, io);
        // firefox's own ROW is gone (its comm cell). Its NAME survives, in its
        // children's parent column — which is the whole point.
        avoid("gate/parent-row-hidden", out, "| firefox |");
        want("gate/parent-still-named", out, "2201 firefox");
        want("gate/child-kept-depth", out, "    \xe2\x94\x94 2281");
        want("gate/says-why", out, "(hidden by the filter / attachable gate)");
    }

    // --- 7. the └ follows the DRAWN siblings, not the snapshot's ---------
    // Hide the youngest child and the elbow has to move up to the one above
    // it, or the tree draws a branch continuing into a row that is not there.
    // Mutation: computing last_sibling over all siblings leaves 2260 with a ├
    // and no └ anywhere at that depth.
    {
        InspectState s;
        std::vector<ProcRow> rows = snapshot();
        rows[5].verdict.verdict = Attach::No; // 2281, the youngest content proc
        rows[5].verdict.why = "already traced by pid 99";
        load(s, std::move(rows));
        const std::string out = render2(s, io);
        avoid("elbow/youngest-gone", out, "2281");
        want("elbow/moved-up", out, "    \xe2\x94\x94 2260");
        avoid("elbow/no-dangling-tee", out, "    \xe2\x94\x9c 2260");
    }

    // --- 8. a non-pid sort greys the tree and SAYS SO --------------------
    // A tree has one order. Clicking "attach" asks for another, and the pane
    // may not silently drop either — it flattens and states the reason.
    //
    // Driven by setting proc_sort_is_pid rather than by synthesising a header
    // click: ImGui persists a table's sort column in its own per-table state,
    // which a null-backend test can only reach by faking input coordinates
    // against a header it cannot see. proc_sort_is_pid IS the pane's contract
    // with itself here — the sort block writes it inside the table, the
    // checkbox and the flattening read it on the next frame — so setting it
    // exercises exactly the state a click produces. What a click would ALSO
    // cover, and this does not, is the sort block's own assignment; that is
    // pinned instead by scenario 1, where the pid sort leaves it true and the
    // tree draws.
    {
        InspectState s;
        load(s, snapshot());
        render(s, io); // frame 1 submits the table and its sort specs
        s.proc_sort_is_pid = false;
        const std::string out = render(s, io);
        want("sort/tree-reason-stated", out,
             "a tree has one order — sort by pid to nest");
        avoid("sort/flattened", out, kElbow);
        // Flattened, but never at the cost of the lineage itself.
        want("sort/parent-column-survives", out, "1130 gnome-shell");
    }

    // --- 9. a cycle still lists both rows --------------------------------
    // Unreachable from a live /proc, reachable from a snapshot read across a
    // recycled pid. The failure it guards is not a mis-drawn row: with the cut
    // removed, neither row is a root, the pre-order walk never reaches them,
    // and a table whose job is to list what is running silently lists neither.
    {
        InspectState s;
        load(s, {pr(10, 11, "a"), pr(11, 10, "b")});
        const std::string out = render2(s, io);
        want("cycle/first-listed", out, "10");
        want("cycle/second-listed", out, "11");
    }

    // --- 10. an empty snapshot draws no lineage claims --------------------
    {
        InspectState s;
        load(s, {});
        const std::string out = render2(s, io);
        avoid("empty/no-connectors", out, kElbow);
        avoid("empty/no-hidden-note", out,
              "(hidden by the filter / attachable gate)");
    }

    ImGui::DestroyContext();

    if (failures) {
        std::fprintf(stderr, "test_procs_draw: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("test_procs_draw: all checks passed\n");
    return 0;
}
