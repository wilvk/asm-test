# Serve-session recording — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Make a *live capture that spans more than one engine* saveable as ONE `.asmtrace` file, so a recording carrying a code plane, a worldline and real kernel crossings can exist at all — which is what Task 7c of [the 3D axis budget plan](2026-08-03-3d-scene-axis-budget.md) is blocked on.

**Architecture:** `--serve` gains a session-level `--record=<path>` sink, opened **once** with **one** header and teed into by every engine the session runs. Today each `start` calls `rec_open` afresh, so a two-engine session emits two headers and `load_recording_file` — which requires the header on line 1 — rejects the stream. The sink is a `shared` writer on the existing `rec_t`, so the "the file and the stream agree by construction" property that struct already guarantees is preserved rather than duplicated.

**Status — ✅ COMPLETE (2026-08-04).** Task 1 landed as `ec611c17`
(`--serve --record=<f>`, `cli/test_serve_record.c` in `cli-smoke`); Task 2 as
`ec378e82` (the frozen fixture + the axis-budget plan's T7c, which is now
closed). Both lanes green; the golden corpus did not move.

**Tech Stack:** C11, `cli/asmspy.c` (the live ptrace tracer), `cli/asmtrace_ndjson.{c,h}` (the NDJSON writer), `desktop/test/` (the null C++ harness), Docker (`asmtest-cli`, `asmtest-desktop`).

## Global Constraints

- **This unblocks 7c; it is not a rewrite of it.** Task 7c's test body, its assertions and its reasoning are already written out in [the axis-budget plan](2026-08-03-3d-scene-axis-budget.md). This plan produces the fixture that task needs and then lands that task verbatim. Do not redesign 7c.
- **Never hand-author or splice a recording.** Concatenating the trace-engine recording and the log-engine recording into one file, or editing headers out, is hand-authoring a container the producer never emitted — the exact thing 7b Step 0 rules out, with extra steps. The producer must emit the file.
- **No gtest.** Every desktop test is a standalone `main()` over a hand-rolled `check(what, cond, why)`; the third argument is the failure explanation, printed only on failure. `cli/` tests are C `main()`s in the same spirit.
- **How to run things.** One desktop test: `make build/desktop_test_<name> && ./build/desktop_test_<name>`. Whole desktop suite: `make desktop-test`. Containerised desktop lane (what CI runs): `make docker-desktop`. CLI lane: `make docker-cli`. Never `make X >/dev/null 2>&1` — it hides compile errors and leaves a stale binary "passing" (this bit during the axis-budget work: a test reported "all checks passed" from a binary whose source had failed to compile).
- **Shared tree.** Many agents work this repo concurrently. Commit by explicit path, never `git add -A`. The shared index is regularly left holding staged deletions of files that exist on disk; repair with a path-scoped `git reset -- <path>` before committing.
- **`tests/golden-asmtrace/` is byte-gated, `desktop/test/fixtures/` is not.** `asmtrace-golden-check` holds the former byte-stable in CI; the latter appears in `mk/desktop.mk` only as a `-DASMTEST_FIXTURE_DIR` define. A live capture is not byte-reproducible, so it belongs in `desktop/test/fixtures/` and is **frozen, never regenerated**.
- **Attaching to a running process inside a container needs `--cap-add=SYS_PTRACE`.** Without it every `start` on an existing pid answers `ptrace/attach failure (permission? ptrace_scope? ...)`. `launch` (fork + `PTRACE_TRACEME`) works without it.

## The measurement this plan rests on

Run in the `asmtest-cli` image against a live target, `--cap-add=SYS_PTRACE`, one `start` per session unless stated. Event kinds counted straight out of the resulting stream:

| serve `mode` | `codeimage` | worldline | `syscall` | note |
|---|---|---|---|---|
| `trace` (`func`, `max:40`) | **1** | **1080 `trace`** | 0 | also 40 `coverage` |
| `log` (`max:120`) | 0 | 0 | **120** | the only syscall producer |
| `stream` (`max:600`) | 0 | 600 `stream` (not `trace`) | 0 | |
| `dataflow` (via `launch`) | — | — | — | `err` before any event |
| `tree` | arms it (`serve_exe_text_span`) | call/tree events | 0 | |

**No single engine emits all three.** `serve_codeimage_arm` is reached only from the `SM_TREE`, `SM_TRACE` and `SM_DATAFLOW` cases (`cli/asmspy.c:4021, :4043, :4048, :4138`); syscalls come only from the `log` engine's own sink (`cli/asmspy.c:997`).

A **two-engine session** does produce every kind — `trace` then `log` in one `--serve` session yields 1 `codeimage` + 1080 `trace` + 120 `syscall`. But it is not a loadable recording: each `start` calls `rec_open` (`cli/asmspy.c:4280`), which writes a header per channel (`rec_open_code`, `cli/asmspy.c:136`), so the stream carries **two** `{"asmtrace":1,...}` lines plus `cmd`/`session` protocol lines, and `load_recording_file` fails on line 1 with `header has no integer "asmtrace" major`.

**`--serve --record=<f>` does not work today, and fails silently.** `main` routes `--serve` to `cmd_serve(NULL)` / `cmd_serve(argv[1] + 8)` (`cli/asmspy.c:8292-8297`) and never examines a later `--record=`; `cmd_serve` takes only a socket path. Observed: no file is created and stderr is empty. That silent no-op is itself a defect this plan closes — `--record=<f>` is documented in `usage()` as applying to "every headless mode", and a flag that is accepted and ignored is worse than one that is rejected.

**Why the session sink is the right fix rather than a test-only shim.** The desktop already treats a live session as ONE growing recording: `shell_sync_live_tab` accumulates every event of a session into `s.ws.recordings[i]` regardless of how many engines ran, which is why the crossings layer works in the GUI at all. The file path is the one that cannot express what the live path already models. This closes that asymmetry, and it gives the product "save this capture" for free.

---

### Task 1: `--serve --record=<path>` — one session, one recording

**Files:**
- Modify: `cli/asmspy.c` — argv routing (`:8292-8297`), `rec_t` (`:68-80`), `rec_emit` (`:156`), `rec_close` (`:215`), `cmd_serve` (`:5166`), the per-engine `rec_open` (`:4280`), `usage()`
- Create: `cli/test_serve_record.c` — the new test
- Create: `examples/serve_record_target.c` — the program it records
- Modify: `mk/cli.mk` — build + run the new test in `cli-smoke`

**Interfaces:**
- Consumes: nothing from other tasks.
- Produces: `asmspy --serve --record=<path>` writes a single `.asmtrace` file containing every event of the whole session, with exactly one header and one `end` footer. `cmd_serve` becomes `static int cmd_serve(const char *sockpath, const char *record)`.

**The shape, and why it is a `shared` writer rather than a second file sink.** `rec_t` already owns the "one body string, two channels, agree by construction" property (its own comment at `cli/asmspy.c:57`). Opening a *second* `rec_t` for the session would re-derive that and give the file its own header per engine — the bug being fixed. Instead the session owns ONE `asmtrace_writer_t`, and each engine's `rec_t` points at it; `rec_emit` tees to it alongside the existing channels.

- [x] **Step 1: Write the failing test**

Create `cli/test_serve_record.c`:

```c
/* test_serve_record.c — a serve session must record as ONE .asmtrace file:
 * one header, one end footer, and the engine's own events, with the wire
 * protocol's control lines left out. Before this, each `start` opened its own
 * recorder and a session that changed engine wrote a header per engine, so the
 * result was not loadable as a recording at all — which is what stopped a
 * capture carrying a code plane, a worldline AND kernel crossings from
 * existing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
static void check(const char *what, int cond, const char *why) {
    if (!cond) {
        fprintf(stderr, "FAIL %s: %s\n", what, why);
        failures++;
    }
}

static int count_lines(const char *path, const char *needle) {
    FILE *f = fopen(path, "r");
    char buf[65536];
    int n = 0;
    if (!f)
        return -1;
    while (fgets(buf, sizeof buf, f))
        if (strstr(buf, needle))
            n++;
    fclose(f);
    return n;
}

static int first_line_is_header(const char *path) {
    FILE *f = fopen(path, "r");
    char buf[65536];
    int ok = 0;
    if (!f)
        return 0;
    if (fgets(buf, sizeof buf, f))
        ok = strstr(buf, "\"asmtrace\":1") != NULL;
    fclose(f);
    return ok;
}

int main(void) {
    const char *out = "build/test-serve-record.asmtrace";
    char cmd[1024];
    remove(out);

    /* `launch` (fork + PTRACE_TRACEME) needs no CAP_SYS_PTRACE, so this runs
     * in any lane. The two-engine union is exercised by the fixture recording
     * in Task 2, which needs an attach. */
    snprintf(cmd, sizeof cmd,
             "{ printf '{\"cmd\":\"launch\",\"mode\":\"trace\","
             "\"argv\":[\"build/serve_record_target\"],\"func\":\"work\","
             "\"max\":20}\\n'; sleep 3; "
             "printf '{\"cmd\":\"stop\"}\\n'; sleep 1; "
             "printf '{\"cmd\":\"quit\"}\\n'; } | "
             "build/asmspy --serve --record=%s >/dev/null 2>&1",
             out);
    if (system(cmd) != 0)
        fprintf(stderr, "note: serve exited non-zero (checked below anyway)\n");

    check("the session wrote a recording at all", access(out, R_OK) == 0,
          "--serve --record produced no file");
    if (access(out, R_OK) != 0) {
        fprintf(stderr, "test_serve_record: %d failure(s)\n", ++failures);
        return 1;
    }

    check("line 1 is the recording header", first_line_is_header(out),
          "load_recording_file reads line 1; a protocol line there makes the "
          "file unloadable");
    check("exactly ONE header for the whole session",
          count_lines(out, "\"asmtrace\":1") == 1,
          "a header per engine is what made a multi-engine capture unloadable");
    check("exactly ONE end footer", count_lines(out, "\"k\":\"end\"") == 1,
          "a footer per engine claims the recording ended more than once");
    check("the wire protocol's control lines stay OUT of the recording",
          count_lines(out, "\"k\":\"cmd\"") == 0 &&
              count_lines(out, "\"k\":\"session\"") == 0,
          "cmd/session are the serve PROTOCOL, not recorded events: the client "
          "stream keeps them, the file must not");
    check("the engine's own events are in the file",
          count_lines(out, "\"k\":\"trace\"") > 0,
          "the recording carries none of the engine's events");

    if (failures) {
        fprintf(stderr, "test_serve_record: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_serve_record: all checks passed\n");
    return 0;
}
```

- [x] **Step 2: Add the target program and register the test**

Create `examples/serve_record_target.c` — `work` is called in a loop so a region-mode trace has entries to sample whether it launched or attached, and its syscalls span three `SyscallClass` families so the SAME binary serves Task 2's crossings fixture:

```c
/* A target for test_serve_record and for the 61 T7c crossings fixture. `work`
 * is called every iteration so a tracer that ATTACHES mid-run still sees
 * entries to it, and the syscalls it makes fall into more than one
 * SyscallClass family — openat/write/close are File, getpid is Process,
 * clock_nanosleep is Time — so the class channel has something to distinguish
 * rather than one colour repeated. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

__attribute__((noinline)) int work(int i) {
    int fd = open("/dev/null", O_WRONLY);
    if (fd >= 0) {
        write(fd, "x", 1);
        close(fd);
    }
    (void)getpid();
    return i + 1;
}

int main(void) {
    int n = 0;
    int i;
    for (i = 0; i < 3000; i++) {
        struct timespec ts = {0, 5000000};
        n = work(n);
        nanosleep(&ts, 0);
    }
    return n & 1;
}
```

In `mk/cli.mk`, beside the other `$(BUILD)/test_*` rules:

```make
$(BUILD)/serve_record_target: examples/serve_record_target.c
	$(CC) $(CFLAGS) -O0 -g $< -o $@

$(BUILD)/test_serve_record: cli/test_serve_record.c
	$(CC) $(CFLAGS) $< -o $@
```

Add `$(BUILD)/test_serve_record $(BUILD)/serve_record_target $(BUILD)/asmspy` to `cli-smoke`'s prerequisites, and in its recipe, after the existing invocations:

```make
	@echo "--- test_serve_record (a serve session is ONE recording) ---"
	$(BUILD)/test_serve_record
```

- [x] **Step 3: Run the test to verify it fails**

Run: `make docker-cli`
Expected: FAIL — `the session wrote a recording at all: --serve --record produced no file`. `--record=` is not parsed in serve mode today.

- [x] **Step 4: Give `rec_t` a shared session writer**

In `cli/asmspy.c`, add to `rec_t` (beside `file`/`out`, `:68`):

```c
    /* A SESSION-level file sink (--serve --record=<path>), owned by the serve
     * loop rather than by this rec_t: one writer, opened once, with ONE
     * header, teed into by every engine the session runs. `file` above cannot
     * serve this — rec_open writes a header per open, so a per-engine file
     * sink produces a header per engine, and a stream with two headers is not
     * a recording (load_recording_file reads line 1). NULL outside serve. */
    asmtrace_writer_t *shared;
```

In `rec_emit` (`:156`), tee to it — and in the paused branch mark it truncated, on the same reasoning the other two channels already are:

```c
    if (r->paused) {
        r->paused_dropped++;
        r->file.truncated = 1;
        r->out.truncated = 1;
        if (r->shared)
            r->shared->truncated = 1;
    } else {
        if (r->have_file)
            asmtrace_emit(&r->file, kind, body);
        if (r->shared)
            asmtrace_emit(r->shared, kind, body);
        if (r->have_out) {
            asmtrace_emit(&r->out, kind, body);
            fflush(r->out.f);
        }
    }
```

Leave `rec_close` (`:215`) otherwise untouched, and say why in a comment there:

```c
/* NB (serve session recording): r->shared is NOT closed here. It belongs to
 * the serve loop, which closes it once at quit — closing it per engine would
 * write an `end` footer per engine and claim the recording ended each time. */
```

- [x] **Step 5: Parse the flag and open the session writer once**

In `main`'s argv routing (`:8292`), replace the two `--serve` branches with one that also accepts `--record=`:

```c
    if (strcmp(argv[1], "--serve") == 0 ||
        strncmp(argv[1], "--serve=", 8) == 0) {
        const char *sock = NULL, *record = NULL;
        int i;
        if (argv[1][7] == '=') {
            if (!argv[1][8])
                return bad_arg("socket path", argv[1]);
            sock = argv[1] + 8;
        }
        /* --record=<f> after --serve records the WHOLE session to one file.
         * Previously accepted and silently ignored, which is worse than a
         * refusal — usage() documents --record for every headless mode. */
        for (i = 2; i < argc; i++) {
            if (strncmp(argv[i], "--record=", 9) == 0 && argv[i][9])
                record = argv[i] + 9;
            else
                return bad_arg("serve option", argv[i]);
        }
        return cmd_serve(sock, record);
    }
```

Change `cmd_serve` (`:5166`) to `static int cmd_serve(const char *sockpath, const char *record)`. Before its command loop, open the session writer ONCE:

```c
    /* The session recording: ONE writer, ONE header, for however many engines
     * this session runs. `pid` is 0 here because a session may outlive any one
     * target (and a `launch` has no pid until its tracer thread forks); the
     * per-engine `session` events on the client stream name the pid, and the
     * header's pid field is not what a reader uses to attribute an event. */
    asmtrace_writer_t sess_file;
    int have_sess = 0;
    if (record) {
        asmtrace_prov_t prov = {"ptrace-serve", 1, "exact", 0, NULL, 0};
        if (asmtrace_open(&sess_file, record, 0) != 0) {
            fprintf(stderr, "--record: cannot write %s: %s\n", record,
                    strerror(errno));
            return 1;
        }
        asmtrace_header(&sess_file, "asmspy", &prov, 0, NULL);
        have_sess = 1;
    }
```

`serve_session_t` gains `asmtrace_writer_t *sess_file;`, set to `have_sess ? &sess_file : NULL` before the loop. At every `return` out of `cmd_serve` — the quit path and each error path — close it exactly once:

```c
    if (have_sess)
        asmtrace_close(&sess_file, 0, 0, NULL);
```

Then at the per-engine `rec_open` (`:4280`), point the engine's `rec_t` at it immediately after the open:

```c
    rec_open(&s->rec, NULL, s->out, backend, exact, trust, s->p.pid);
    s->rec.shared = s->sess_file; /* NULL when the session is not recording */
```

**The control lines must not reach the file.** `cmd`/`session`/`err` go through `serve_emitf`, which writes the client stream rather than the engine's `rec_t`. Read `serve_emitf` before trusting that; the test's zero-`cmd`/zero-`session` assertion is what catches a `serve_emitf` that also tees.

- [x] **Step 6: Run the test to verify it passes**

Run: `make docker-cli`
Expected: PASS, including `test_serve_record: all checks passed`, and `asmtrace-golden-check: 33 recordings byte-identical to the corpus` **unchanged** — this task adds nothing to `asmtrace_record.c`, so no golden may move. If one does, that is a real finding, not churn to accept.

- [x] **Step 7: Update `usage()`**

The `--record=<f>` paragraph says it applies to "every headless mode". Extend it in the same voice:

```
--record=<f> writes a .asmtrace NDJSON recording of the run (every
headless mode), and after --serve records the WHOLE session — every
engine it runs — as ONE recording, so a capture that changes engine is
still a single loadable file.
```

- [x] **Step 8: Commit**

```bash
git add cli/asmspy.c cli/test_serve_record.c examples/serve_record_target.c mk/cli.mk
git commit -m "asmspy: record a whole serve session as one recording"
```

---

### Task 2: Freeze the crossings fixture and land 7c

**Files:**
- Create: `desktop/test/fixtures/motif-crossings.asmtrace` — one frozen capture
- Create: `desktop/test/fixtures/syscall_target.c` — the recorded program, committed beside the fixture so its provenance is readable rather than only described
- Modify: `desktop/test/test_crossing.cpp` — the real-capture block
- Modify: `mk/desktop.mk` — `test_crossing.o` needs the `ASMTEST_FIXTURE_DIR` define it does not have today

**Interfaces:**
- Consumes: Task 1's `--serve --record=<path>`.
- Produces: no signature change. A regression gate over real data.

- [x] **Step 1: Record the fixture**

Copy `examples/serve_record_target.c` to `desktop/test/fixtures/syscall_target.c` so the fixture and its source sit together. Its syscalls span three `SyscallClass` families — `openat`/`write`/`close` are File, `getpid` is Process, `clock_nanosleep` is Time — which is what makes the class channel checkable rather than a single-colour smear. Verified against `class_of` (`desktop/src/views/crossing.cpp:45`): all five names are in its vocabulary.

Two engines, because no single one emits every kind: `trace` arms the codeimage and records the worldline, `log` records the syscalls.

```bash
docker run --rm --cap-add=SYS_PTRACE --security-opt seccomp=unconfined \
  -v "$PWD:/w" -w /w asmtest-cli:latest bash -c '
cc -O0 -g -o /tmp/t desktop/test/fixtures/syscall_target.c
/tmp/t & TPID=$!
sleep 0.3
{ printf "{\"cmd\":\"start\",\"mode\":\"trace\",\"pid\":%d,\"func\":\"work\",\"max\":40}\n" "$TPID"
  sleep 5; printf "{\"cmd\":\"stop\"}\n"; sleep 1
  printf "{\"cmd\":\"start\",\"mode\":\"log\",\"pid\":%d,\"max\":120}\n" "$TPID"
  sleep 5; printf "{\"cmd\":\"quit\"}\n"; } |
  build/asmspy --serve --record=desktop/test/fixtures/motif-crossings.asmtrace \
  >/dev/null
kill $TPID 2>/dev/null'
docker run --rm -v "$PWD:/w" -w /w asmtest-cli:latest \
  chown -R "$(id -u):$(id -g)" desktop/test/fixtures
```

Then **verify the shape before committing it** — a fixture missing any of the three kinds makes every assertion below fail for a misleading reason:

```bash
grep -c '"asmtrace":1' desktop/test/fixtures/motif-crossings.asmtrace   # want 1
grep -o '"k":"[a-z]*"' desktop/test/fixtures/motif-crossings.asmtrace |
  sort | uniq -c | sort -rn | head
```

Expected from the measured run: 1 header, 1 `codeimage`, ~1080 `trace`, ~120 `syscall`, ~40 `coverage`, 1 `end`; roughly 100 KB.

**Frozen, never regenerated.** `desktop/test/fixtures/` has no byte-check gate, which is exactly why a non-byte-reproducible live capture belongs there — the precedent `test_scene_fbo` set by reusing `obs-survey-ibs.asmtrace`.

- [x] **Step 2: Add the test block**

Add to `desktop/test/test_crossing.cpp` the block written out verbatim in the axis-budget plan's Task 7c Step 2 — the shape precondition (codeimage / non-empty trace / at least two syscalls), then `build_crossing_layer` over the capture, then the two contract checks: spurs exist, and not every spur classifies as `Other`.

Three mechanical points, already recorded in that plan, that must not be re-derived wrong:

- `load_recording_file` returns `std::optional<Recording>` (`desktop/src/doc/recording.h:134`). Test it with `rec.has_value()`, never against `nullptr`, and add `#include <optional>`.
- Add the `ASMTEST_FIXTURE_DIR` `#error` guard at the top of the file, matching every other fixture-reading test.
- Record the provenance in the test's own comment: the exact command from Step 1, and the fact that two engines were needed because no single one emits all three kinds.

- [x] **Step 3: Register the fixture define**

`test_crossing.o` has no `ASMTEST_FIXTURE_DIR` today. Add it in `mk/desktop.mk` beside the others (`:513-516`):

```make
# 61 T7c: the crossing channel's real-capture fixture (a frozen asmspy run).
$(BUILD)/desktop/test/t/test_crossing.o: DESKTOP_TEST_EXTRA = -DASMTEST_FIXTURE_DIR='"desktop/test/fixtures"'
```

The link rule (`:1397`) and the `DESKTOP_TESTS` entry already exist, and the loader is already linked (that rule ends in `$(DESKTOP_TEST_DOC)`), so the define is the only makefile change.

- [x] **Step 4: Run**

Run: `make build/desktop_test_crossing && ./build/desktop_test_crossing`
Expected: PASS, including every pre-existing NDJSON block — run the whole file.

**If "the class channel distinguishes at least one real syscall" fails, do not relax it.** Either the capture caught only syscalls `class_of` has no word for — re-record with the target above, which makes `openat`/`write`/`getpid` — or it abstains where it should not, which is a real D7 finding to report.

- [x] **Step 5: Run the container lane**

Run: `make docker-desktop`
Expected: PASS. Judge from the container: `test_scene_fbo`'s "T3 GL contour bands" check fails on a host with a real GPU but passes under the container's llvmpipe.

- [x] **Step 6: Commit**

```bash
git add desktop/test/fixtures/motif-crossings.asmtrace \
        desktop/test/fixtures/syscall_target.c \
        desktop/test/test_crossing.cpp mk/desktop.mk
git commit -m "desktop(test): pin the crossing class channel against a real capture"
```

---

## Self-review

**Spec coverage.** The blocker is that no loadable recording can carry a code plane, a worldline and kernel crossings at once. Task 1 makes such a recording emittable (`--serve --record`); Task 2 records one, freezes it, and lands the axis-budget plan's Task 7c against it. After Task 2, 7c can be ticked and Task 10 is the only one of that plan left.

**Type consistency.** `cmd_serve` gains its second parameter in Task 1 Step 5 and is called only from `main`'s routing in that same step. `rec_t::shared` is an `asmtrace_writer_t *`, declared in Step 4, assigned in Step 5, read only in `rec_emit`; `serve_session_t::sess_file` is the same pointer type. `asmtrace_open` / `asmtrace_header` / `asmtrace_emit` / `asmtrace_close` are the existing `cli/asmtrace_ndjson.h` API, used exactly as `rec_open_code` and `rec_close` already use them. `examples/serve_record_target.c` and `desktop/test/fixtures/syscall_target.c` are the same program, and Task 2 Step 1 says to copy it rather than write a second one.

**What this plan deliberately does NOT do.**

- It does not make one engine emit all three kinds. Teaching the `log` engine to arm a codeimage, or the region tracer to record syscall stops, changes those engines' own contracts and is not needed: the session sink is what the desktop's live path already models, and it delivers "save this capture" as a product feature besides.
- It does not touch `tools/asmtrace_record.c`. That producer is a Unicorn emulator over hand-assembled bytes and never enters a kernel, so it can never emit a `syscall` row — which is why 7c's fixture has to come from the live tracer at all.
- It does not weaken 7c. If Task 1 proves larger than it looks, the honest fallback is to report that and leave 7c open — **not** to splice two recordings into one file, which is hand-authoring the container the test exists to avoid.

**Open risk.** Task 1's test asserts a *single-engine* session records correctly (one header, one footer, engine events present, no protocol lines). It does not prove the two-engine union, because attaching to a second target needs `CAP_SYS_PTRACE`, which the CI lane may not grant. Task 2 Step 1's shape check is the gate on the two-engine path. If a CI-visible two-engine test is wanted later, the way to get it is a second `launch` in the same session rather than an attach.
