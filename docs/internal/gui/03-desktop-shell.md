# Desktop shell: app skeleton, deps, build integration, document model — implementation

> **Sources.** Actioned from [desktop-gui-plan.md](../plans/desktop-gui-plan.md)
> — §D5 "Stack and process model", §D7 "Licensing", "DX: how the GUI itself is
> built and tested", the Phase 2 viewer entry — under decisions D1–D11 in this
> directory's [README.md](README.md); siblings are linked where referenced.
> Written 2026-07-23. If this doc and a source disagree, this doc wins (sources
> may be stale); if the CODE and this doc disagree, re-verify before
> implementing.

## Why this work exists

Every view doc (04–09) needs a place to render into and a model to render from. This doc builds exactly that: the `desktop/` tree, pinned Dear ImGui + nlohmann/json fetched the way the repo pins DynamoRIO, `mk/desktop.mk` + `Dockerfile.desktop`, two binaries (full app, GPL-2.0 as a whole per D4; render-only viewer with zero engine deps), the `.asmtrace` document model enforcing the plan-D1 forward-compat and honesty rules, the reuse seam for the asmspy view-model headers, and the golden-recording smoke. Afterwards a view doc adds a `desktop/src/ui/*.cpp` file and a tab — no build or loader work. Two facts below were established by compiling, not reading: the asmspy headers do **not** build as C++17 as the plan implies (T4/T5 fix it), and the autoregion include-order dependency the plan's D5 records is real.

## What already exists (verified 2026-07-23)

- [scripts/fetch-dynamorio.sh](../../../scripts/fetch-dynamorio.sh) — the T1 pattern: pinned version default ([:15](../../../scripts/fetch-dynamorio.sh#L15)), digest helpers sourced ([:17](../../../scripts/fetch-dynamorio.sh#L17)), idempotent cache reuse ([:24](../../../scripts/fetch-dynamorio.sh#L24)), refuse-unpinned / fail-on-mismatch ([:36–47](../../../scripts/fetch-dynamorio.sh#L36)), license captured on first fetch ([:60–65](../../../scripts/fetch-dynamorio.sh#L60)), key-file check ([:67](../../../scripts/fetch-dynamorio.sh#L67)), home echoed ([:68](../../../scripts/fetch-dynamorio.sh#L68)). Helpers `tp_digest`/`tp_sha256`: [lib-thirdparty.sh:11](../../../scripts/lib-thirdparty.sh#L11)/[:20](../../../scripts/lib-thirdparty.sh#L20).
- [scripts/third-party-digests.txt](../../../scripts/third-party-digests.txt) — format ([:12](../../../scripts/third-party-digests.txt#L12)), GitHub source-archive row to copy (unicorn, [:31](../../../scripts/third-party-digests.txt#L31)), self-hashed precedent ([:23](../../../scripts/third-party-digests.txt#L23)). **Caution:** [refresh-thirdparty-digests.sh](../../../scripts/refresh-thirdparty-digests.sh) rewrites the manifest in full ([:9](../../../scripts/refresh-thirdparty-digests.sh#L9)) from a hardcoded row list ([:147–166](../../../scripts/refresh-thirdparty-digests.sh#L147)) that already omits the hand-added rows (libipt, apache-maven, pin-3.20, libdft64, binfmt) — add T1's rows **by hand**; never run the refresh script.
- [Dockerfile.cli](../../../Dockerfile.cli) — the image shape T2 mirrors: `ARG BASE_IMAGE` + `FROM` ([:18–19](../../../Dockerfile.cli#L18)), apt of pinned distro dev packages ([:21–24](../../../Dockerfile.cli#L21)), smoke `CMD` ([:26](../../../Dockerfile.cli#L26)). The base carries build-essential ([Dockerfile.bindings-base:28](../../../Dockerfile.bindings-base#L28)) plus libunicorn and pinned-source Keystone/Capstone ([:44](../../../Dockerfile.bindings-base#L44)) — the full app links with no further engine installs.
- [mk/cli.mk](../../../mk/cli.mk) — pkg-config missing-dep probe ([:32–38](../../../mk/cli.mk#L32)), container-guidance recipe ([:100–110](../../../mk/cli.mk#L100)), headless test rules ([test_view :301–303](../../../mk/cli.mk#L301), [test_graphsort :307–309](../../../mk/cli.mk#L307)), the `docker-cli` rule `docker-desktop` mirrors ([:407–411](../../../mk/cli.mk#L407)). Docker vars: `DOCKER` ([mk/docker.mk:23](../../../mk/docker.mk#L23)), `_docker_plat` ([:31](../../../mk/docker.mk#L31)), `DOCKER_BINDINGS_BASE` ([:218](../../../mk/docker.mk#L218)), base rule ([:293–295](../../../mk/docker.mk#L293)).
- [Makefile](../../../Makefile) — hand-maintained `help` ([:91](../../../Makefile#L91); bindings group ends [:175](../../../Makefile#L175), Docker group starts [:176](../../../Makefile#L176)); include point ([`include mk/cli.mk` :929](../../../Makefile#L929), bindings [:930](../../../Makefile#L930)); `FRAMEWORK_OBJS` ([:52](../../../Makefile#L52)); Author-tier link precedent `test_emu` ([:882–886](../../../Makefile#L882)), `UNICORN_LIBS` ([:842](../../../Makefile#L842)), `KEYSTONE_LIBS` ([:898](../../../Makefile#L898)), `assemble.o` ([:900–902](../../../Makefile#L900)); the CI-gated format list desktop/ stays OUT of ([FMT_SOURCES :776–783](../../../Makefile#L776), [fmt-check :787–788](../../../Makefile#L787); D8).
- [mk/bindings.mk](../../../mk/bindings.mk) — `CXX`/`CXXFLAGS ?=` ([:122–123](../../../mk/bindings.mk#L122)); included **after** the T2 include point, so `mk/desktop.mk` expands `$(CXX)`/`$(CLANG_FORMAT)` lazily (recipes only, never `:=`). Recipe-time fetch-call precedent: [:340](../../../mk/bindings.mk#L340).
- [cli/asmspy.h](../../../cli/asmspy.h) — includes `<stdatomic.h>` ([:22](../../../cli/asmspy.h#L22)) and `asmspy_treefilter.h` ([:27](../../../cli/asmspy.h#L27)); `asmspy_gnode_t` ([:365](../../../cli/asmspy.h#L365)); `asmspy_sample_edge_t` ([:509](../../../cli/asmspy.h#L509)). No `extern "C"` guards (unlike [include/asmtest_ptrace.h:57](../../../include/asmtest_ptrace.h#L57)) — safe only because the desktop consumes header-inline functions and never links engine symbols (D9). **Verified by compilation today:** `g++ -std=c++17` fails on `atomic_bool` (no C11-atomics alias in C++ < 23); with `#include <atomic>` + `using std::atomic_bool;` first, asmspy.h + logview/dataview/autoregion compile clean.
- [cli/asmspy_graphsort.h](../../../cli/asmspy_graphsort.h) — the file-scope qsort latch ([:29](../../../cli/asmspy_graphsort.h#L29)) the plan's D5 flags, plus — verified by compilation today — a second C++ blocker: the implicit `const void*` conversion at [:31](../../../cli/asmspy_graphsort.h#L31). T4 fixes both. [cli/asmspy_autoregion.h](../../../cli/asmspy_autoregion.h) uses `asmspy_sample_edge_t` in signatures ([:124–128](../../../cli/asmspy_autoregion.h#L124)) without including asmspy.h — the include-order dependency, reproduced today; resolve-callback shape at [:83](../../../cli/asmspy_autoregion.h#L83).
- [cli/test_graphsort.c](../../../cli/test_graphsort.c) — sets the latch, then qsorts ([:33–34](../../../cli/test_graphsort.c#L33)); T4 extends it. [cli/test_view.c](../../../cli/test_view.c) — the failures-counter + `check_*` style ([:23–41](../../../cli/test_view.c#L23)) the desktop tests copy.
- [licenses/README.md](../../../licenses/README.md) — the table ([:9](../../../licenses/README.md#L9)) T1 extends. [bindings/conformance/](../../../bindings/conformance/) seeds the golden corpus via 01's `make asmtrace-golden` (D6); `tests/golden-asmtrace/` does **not exist yet** — T7 waits on 01. No `desktop/` dir, no `.asmtrace` reference in `mk/` or `cli/` — everything below is greenfield.

## Tasks

Everything named `asmdesk::*`, every `desktop/*` file, `mk/desktop.mk`, `Dockerfile.desktop`, the two fetch scripts, and targets `desktop`/`desktop-render`/`desktop-test`/`docker-desktop` are **new** (introduced here). `make asmtrace-golden`, `tests/golden-asmtrace/`, and [asmtrace-schema.md](asmtrace-schema.md) are **new, owned by 01**.

### T1 — Pinned fetch scripts, digests, licenses  (S, depends on: none)

**Goal.** `scripts/fetch-imgui.sh` + `scripts/fetch-json.sh` mirroring `fetch-dynamorio.sh` exactly; digest rows; both licenses vendored + recorded.

**Steps.**
1. Write `scripts/fetch-imgui.sh` (Code): Dear ImGui v1.91.9 source archive → `build/imgui/imgui-1.91.9`, digest-verified, license captured to `licenses/DearImGui-LICENSE.txt` on first fetch, home echoed.
2. Write `scripts/fetch-json.sh`: release asset `https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp` → `build/nlohmann-json/3.11.3/nlohmann/json.hpp` (the subdir makes `#include <nlohmann/json.hpp>` work with one `-I`). Same digest discipline; one file — hash it directly, no tar step.
3. Compute both SHA-256es locally (`curl -fsSL <url> | sha256sum`) and append two rows **by hand** (refresh-script caution above), with a comment citing the self-hashed precedent ([third-party-digests.txt:23](../../../scripts/third-party-digests.txt#L23)):
   ```
   # imgui + nlohmann-json: the desktop GUI (docs/internal/gui/03-desktop-shell.md).
   # Both MIT, both compiled INTO the desktop binaries (bundled). Self-hashed at pin time.
   tarball-sha256  imgui          1.91.9  sha256:<computed>
   tarball-sha256  nlohmann-json  3.11.3  sha256:<computed>
   ```
4. Run each script once; commit the captured `DearImGui-LICENSE.txt`. json.hpp carries no license text — commit `licenses/nlohmann-json-LICENSE.MIT` directly (verbatim `LICENSE.MIT` from the `v3.11.3` tag).
5. Add two rows to the [licenses/README.md](../../../licenses/README.md#L9) table: both **bundled + permissive (MIT)**, fetched by their scripts; MIT adds no constraint beyond D4's split (full app effectively GPL-2.0 via the engines; viewer stays permissive).

**Code.** `fetch-imgui.sh` skeleton (fetch-json.sh identical minus tar):

```sh
#!/bin/sh
# fetch-imgui.sh — pinned Dear ImGui release into build/ (mirrors
# fetch-dynamorio.sh). Prints IMGUI_HOME on stdout. Idempotent. Any OS.
set -eu
IMGUI_VERSION="${IMGUI_VERSION:-1.91.9}"
IMGUI_URL="${IMGUI_URL:-https://github.com/ocornut/imgui/archive/refs/tags/v${IMGUI_VERSION}.tar.gz}"
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib-thirdparty.sh"
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
home="${IMGUI_CACHE:-$root/build/imgui}/imgui-$IMGUI_VERSION"
if [ ! -e "$home/imgui.cpp" ]; then
    : # mkdir; curl-or-wget to $tmp; then EXACTLY the fetch-dynamorio.sh:36-47
      # verify block with `tp_digest tarball-sha256 imgui "$IMGUI_VERSION"`
      # (refuse-unpinned + mismatch-fail wording unchanged); then tar -xzf.
fi
lic="$root/licenses/DearImGui-LICENSE.txt"
[ -f "$lic" ] || cp "$home/LICENSE.txt" "$lic"
[ -f "$home/backends/imgui_impl_glfw.cpp" ] || { echo "fetch-imgui: ERROR: backends missing" >&2; exit 1; }
echo "$home"
```

**Tests.** Run each script twice: fetch+verify, then `reusing cached` with the same stdout. Corrupt one digest byte → FAIL printing expected/got.

**Docs.** The licenses/README.md rows.

**Done when.**
- Both scripts exit 0 twice (fetch, then cache-reuse) and echo their homes.
- A tampered digest row makes the matching script exit non-zero.
- Two license files + two README rows committed; nothing under `build/` is (D1).

### T2 — mk/desktop.mk + Dockerfile.desktop + null-backend smoke  (M, depends on: T1)

**Goal.** The build skeleton: all four targets, Docker lane, Makefile include + help lines, and a headless test proving ImGui renders with no display/GL (the `example_null` pattern — verify at `$(IMGUI_HOME)/examples/example_null/main.cpp` on first fetch).

**Steps.**
1. Create `mk/desktop.mk` (Code). Add `include mk/desktop.mk       # desktop GUI: ImGui shell + .asmtrace viewer` immediately after [`include mk/cli.mk` (:929)](../../../Makefile#L929).
2. Create `desktop/test/test_null_render.cpp`: `ImGui::CreateContext`; font atlas via `io.Fonts->GetTexDataAsRGBA32(&px,&w,&h)`; 3 × (`NewFrame`, one trivial `Begin`/`End` window, `Render`); `DestroyContext`; print `test_null_render: PASS`. No backend, no GLFW.
3. Create `Dockerfile.desktop` mirroring [Dockerfile.cli:18–26](../../../Dockerfile.cli#L18): same `ARG BASE_IMAGE=asmtest-bindings-base`/`FROM`, apt `libglfw3-dev libgl1-mesa-dev` (pinned distro packages; build-essential is in the base), `COPY . .`, `CMD sh -c 'make desktop desktop-render desktop-test'`. (Until T3/T5/T6 land, the app targets compile ImGui + a stub `main.cpp`.)
4. Help lines: a new group between [Makefile:175](../../../Makefile#L175) and [:176](../../../Makefile#L176) — `desktop`, `desktop-render`, `desktop-test` — plus a `docker-desktop` line in the Docker group.
5. D8: `desktop-fmt` / `desktop-fmt-check` over `desktop/**` using `$(CLANG_FORMAT)`; the check recipe is `-`-prefixed (informational, never gates). Do **not** touch [FMT_SOURCES](../../../Makefile#L776).

**Code.** `mk/desktop.mk` load-bearing lines:

```make
# desktop.mk — desktop GUI: full app + render-only viewer + headless tests
# (docs/internal/gui/03-desktop-shell.md). Included BEFORE mk/bindings.mk, so
# $(CXX)/$(CLANG_FORMAT) are referenced lazily (recipes only, never :=).
IMGUI_VERSION ?= 1.91.9
IMGUI_HOME    ?= $(BUILD)/imgui/imgui-$(IMGUI_VERSION)
JSON_VERSION  ?= 3.11.3
JSON_HOME     ?= $(BUILD)/nlohmann-json/$(JSON_VERSION)
DESKTOP_CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -g -Icli -Iinclude \
  -Idesktop/src -I$(IMGUI_HOME) -I$(IMGUI_HOME)/backends -I$(JSON_HOME)

$(IMGUI_HOME)/imgui.cpp: scripts/fetch-imgui.sh scripts/third-party-digests.txt
	sh scripts/fetch-imgui.sh >/dev/null
$(JSON_HOME)/nlohmann/json.hpp: scripts/fetch-json.sh scripts/third-party-digests.txt
	sh scripts/fetch-json.sh >/dev/null

# THREE object trees — shared sources compile per-binary (render adds
# -DASMTEST_DESKTOP_RENDER_ONLY=1; test has no backends), so .o are never
# shared: $(BUILD)/desktop/{app,render,test}/%.o. Pattern rules cover
# desktop/src{,/doc,/ui}, desktop/test, $(IMGUI_HOME)/*.cpp and
# backends/imgui_impl_{glfw,opengl3}.cpp; all order-only depend on the two
# fetch stamps above.

DESKTOP_MISSING :=                       # mirror mk/cli.mk:32-38
ifneq ($(shell pkg-config --exists glfw3 2>/dev/null && echo ok),ok)
DESKTOP_MISSING += libglfw3-dev
endif
GLFW_LIBS ?= $(shell pkg-config --libs glfw3 2>/dev/null || echo -lglfw)
GL_LIBS   ?= -lGL

# desktop        -> $(BUILD)/asmtest-desktop: imgui core + glfw/opengl3
#   backends + doc/ + ui/ + vm_compat + Author-tier objects ($(BUILD)/emu.o
#   $(BUILD)/trace.o $(BUILD)/disasm.o $(BUILD)/assemble.o $(BUILD)/dataflow.o —
#   Makefile:882-886 precedent), linked with $(UNICORN_LIBS) $(KEYSTONE_LIBS)
#   $(CAPSTONE_LIBS) $(GLFW_LIBS) $(GL_LIBS). GPL-2.0 as a whole (D4).
#   NOTE (verified by linking): $(FRAMEWORK_OBJS) is NOT linked — asmtest.o is the
#   test-runner and defines its own main() (src/asmtest.c:2234), which collides
#   with the desktop's main.cpp. The five engine objects above are self-contained
#   and carry the GPL engine linkage on their own.
# desktop-render -> $(BUILD)/asmtest-viewer: same ui/doc/vm_compat sources with
#   -DASMTEST_DESKTOP_RENDER_ONLY=1; links ONLY imgui + backends + $(GLFW_LIBS)
#   $(GL_LIBS). ZERO engine objects or libs on the link line.
# desktop-test   -> doc/ + test mains + imgui core only (no backends, no GLFW,
#   no GL, no engines): runs on ANY host with a C++17 compiler.
# Non-empty DESKTOP_MISSING: desktop/desktop-render print mk/cli.mk:100-110-
# style guidance (apt line, or `make docker-desktop`) and `false`;
# desktop-test never gates on GLFW.

.PHONY: docker-desktop                   # mirror mk/cli.mk:407-411
docker-desktop: docker-bindings-base
	$(DOCKER) build $(_docker_plat) -f Dockerfile.desktop \
	  --build-arg BASE_IMAGE=$(DOCKER_BINDINGS_BASE) -t asmtest-desktop .
	$(DOCKER) run --rm $(_docker_plat) asmtest-desktop
```

**Tests.** `make desktop-test` builds `$(BUILD)/desktop_test_null` from `test_null_render.cpp` + the four ImGui core TUs (`imgui.cpp`, `imgui_draw.cpp`, `imgui_tables.cpp`, `imgui_widgets.cpp`) and runs it; `make docker-desktop` runs the image CMD.

**Docs.** Help lines (step 4); target comments in mk/desktop.mk.

**Done when.**
- `make desktop-test` prints `test_null_render: PASS` on a bare host with only g++ (no GLFW, no display, no engines).
- `make docker-desktop` exits 0; `make help` lists the four new targets.
- Without GLFW, `make desktop` prints guidance and fails — no raw compiler errors.

### T3 — Recording/Workspace model + NDJSON loader + dishonesty fixtures  (M, depends on: T1, T2)

**Goal.** The document model every view consumes: `.asmtrace` NDJSON → `Recording` (events grouped by kind + mandatory provenance), a `Workspace` set of them (plan D3: every view accepts one or two), forward-compat + honesty rules enforced.

> **Schema reconciliation (code wins, done in this change).** An earlier draft of
> this task assumed a `"kind"` event field, a `provenance.truncated`/`drops`
> shape, and a "torn tail = unparseable final line while truncated" rule. The
> **shipped 01 schema/writer** (verified against
> [asmtrace-schema.md](asmtrace-schema.md) and `tests/golden-asmtrace/`) instead
> uses: the event-kind field is **`"k"`**; the header carries a top-level
> **`producer` `{name,version}`** plus a mandatory **`provenance`
> `{backend,exact,trust,redacted?}`** (no `truncated`/`drops`); **truncation and
> drops ride on the `end` footer**; and **a recording with no `end` event is
> TORN**. The loader below and `recording.h`/`.cpp` implement the shipped schema;
> this is the reconciliation the README's "code wins, fix the doc in the same
> change" rule requires.

**Steps.**
1. Create `desktop/src/doc/recording.h`/`.cpp` and `workspace.h`/`.cpp` (Code). Field names follow [asmtrace-schema.md](asmtrace-schema.md) (owned by 01) — 01 wins on names, this doc wins on loader behavior.
2. Loader rules, exactly:
   - Line 1 must parse as a JSON object with integer `"asmtrace"`; else error.
   - `"asmtrace" > 1` → error naming both majors (reject newer major, by name).
   - Header must contain a `"provenance"` object → else error (D7: a stream without provenance is not a recording). Within it, `"exact"` is mandatory and **must not be defaulted** (schema) → a missing/non-bool `exact` is a reject. `backend`/`trust`/`redacted` are read leniently; the top-level `producer` and `arch` are read when present.
   - Every event line must be a JSON object with string **`"k"`** → else error with line number. Exception: an unparseable **final** line (EOF with no trailing newline — a producer that died mid-write) is kept as `torn = true`, never silently dropped; an unparseable **non-final** line is a hard error.
   - The `end` footer (`"k":"end"`) is lifted into the honesty fields, not stored in `by_kind`: `end.truncated` → `end_truncated`, `end.drops.{lost,throttled}` → `drops_*`, `end.events` → `declared_events` (a self-check). **No `end` event seen ⇒ `torn = true`** (schema: a file without an `end` is TORN).
   - Unknown kinds load fine: grouped under their kind string in `by_kind`, counted in `unknown_kinds` (the v1 registry is the 16 kinds incl. `end`; the schema's *reserved* kinds are unknown to a v1 reader too). Unknown fields stay in `Event::body`, ignored. Never an error (forward compat).
   - D10: `trace`/`dataflow` events MAY carry a `"disasm"` string; it rides untouched in `Event::body` — absence is normal (views degrade to offsets).
3. Commit hand-written fixtures under `desktop/test/fixtures/`, one per rule: `min-trace.asmtrace` (valid; one `trace` with `disasm`, one without), `truncated.asmtrace` (`end.truncated:true` + a truncated `coverage`), `dropped.asmtrace` (statistical `survey` + `end.drops.lost>0`/`throttled`), `redacted.asmtrace` (syscall events, no `payload` field, `provenance.redacted:true`), `unknown-kind.asmtrace` (an out-of-registry `k` **and** a known kind with an extra field), `newer-major.asmtrace` (`{"asmtrace": 99, …}`), `missing-provenance.asmtrace`, `torn-tail.asmtrace` (valid lines then a cut final line, no trailing newline).
4. Create `desktop/test/test_recording.cpp` in the [test_view.c](../../../cli/test_view.c#L23) style (failures counter, `check_*` helpers, `PASS` line); wire into `desktop-test`.

**Code.**

```cpp
// desktop/src/doc/recording.h  (namespace asmdesk; all new)
inline constexpr int kAsmtraceMajor = 1;

struct Producer { std::string name, version; };   // header's top-level producer
struct Provenance {              // mandatory on every stream (schema Provenance)
    std::string backend;         // measured producer id, e.g. "ptrace-syscalls"
    bool exact = true;           // true=every event observed; false=a sample
    std::string trust;           // "exact"|"statistical"|"weak"|"strong"
    bool redacted = false;       // payload withheld at RECORD time
    nlohmann::json raw;          // full object, for provenance chrome / annex
};
struct Event {
    std::string kind;            // the schema "k" selector
    nlohmann::json body;         // whole line; views decode lazily (D10 disasm rides here)
};
struct Recording {
    int version = 0;
    Producer producer;
    Provenance provenance;
    std::string arch;
    std::map<std::string, std::vector<Event>> by_kind;   // never holds `end`
    uint64_t unknown_kinds = 0;  // events kept but not in the v1 registry
    // honesty facts off the `end` footer (or its absence):
    bool has_end = false;
    uint64_t declared_events = 0;  // footer's own count (a self-check)
    bool end_truncated = false;
    uint64_t drops_lost = 0;
    bool drops_throttled = false;
    bool torn = false;             // no `end` event -> TORN
    std::string path;
    bool truncated() const { return end_truncated || torn; }
    bool dropped() const { return drops_lost > 0 || drops_throttled; }
    bool statistical() const { return !provenance.exact; }
    uint64_t event_count() const;  // sum of by_kind sizes
};
// Empty optional + err (with line number) on the reject rules; else success.
std::optional<Recording> load_recording(std::istream &in, std::string &err);
std::optional<Recording> load_recording_file(const std::string &path,
                                             std::string &err);

// desktop/src/doc/workspace.h — plan D3: the document model is a SET of
// recordings; every view takes one or two.
struct Workspace {
    std::vector<Recording> recordings;
    int open(const std::string &path, std::string &err); // index, or -1
    void close(size_t idx);
};
```

**Tests.** `test_recording.cpp` cases, one per rule: `load_minimal_groups_by_kind`, `disasm_optional_passthrough`, `reject_newer_major` (names the major), `reject_missing_provenance`, `reject_missing_exact` (never defaulted), `reject_kindless_event` (with line number), `reject_unparseable_midline` (a cut MIDDLE line errors), `unknown_kind_kept_and_counted`, `unknown_field_ignored`, `truncation_surfaces_on_recording`, `drops_surface`, `redacted_flag_survives`, `torn_tail_kept` (cut final line ⇒ `torn`, load succeeds), `torn_missing_end` (no footer ⇒ `torn`), `declared_events_matches_count`, `workspace_opens_a_set`. Fixture dir via a compile define set by the make rule.

**Docs.** Header comments; the fixtures are self-documenting.

**Done when.**
- `make desktop-test` runs `test_recording` green on a bare host and inside `make docker-desktop`.
- Each dishonesty fixture produces its asserted outcome (error vs surfaced flag) — the D7 laws are executable, not prose.

### T4 — graphsort comparator-context lift + C++ compat  (S, depends on: none)

**Goal.** Make [cli/asmspy_graphsort.h](../../../cli/asmspy_graphsort.h) safe for multi-panel GUI use and valid C++ (both blockers verified today) without changing TUI behavior.

**Steps.**
1. Lift the comparison into a context-explicit pure core; keep the latch as a qsort(3) adapter (Code). The explicit casts fix the C++ `const void*` error at [:31](../../../cli/asmspy_graphsort.h#L31) as a side effect.
2. Header-comment rule: **latch + `gnode_cmp` are for single-threaded qsort call sites only (the TUI); concurrent consumers (GUI panels) call `gnode_cmp_key` directly** (e.g. `std::sort` + lambda), never the latch.
3. Extend [cli/test_graphsort.c](../../../cli/test_graphsort.c): direct `gnode_cmp_key` sign/antisymmetry checks for both keys with **no latch assignment**, plus one case asserting adapter and core agree on the same pair. Existing latch-driven cases stay (they now pin the adapter).
4. `make cli-smoke` (at minimum `$(BUILD)/test_graphsort`, rule at [mk/cli.mk:307–309](../../../mk/cli.mk#L307)) stays green — no ordering change.

**Code.**

```c
/* Context-explicit core: pure, total order, safe from any thread. */
static inline int gnode_cmp_key(const asmspy_gnode_t *x,
                                const asmspy_gnode_t *y, gsort_t key) {
    /* body of today's gnode_cmp with `graph_sort_key` replaced by `key` */
}
/* qsort(3) adapter over the file-scope latch — single-threaded call sites
 * (the TUI) only; GUI panels use gnode_cmp_key. */
static gsort_t graph_sort_key = GSORT_INVOCATIONS;
static int gnode_cmp(const void *a, const void *b) {
    return gnode_cmp_key((const asmspy_gnode_t *)a,
                         (const asmspy_gnode_t *)b, graph_sort_key);
}
```

**Tests.** Step 3. **Docs.** The header-comment rule (step 2).

**Done when.**
- `$(BUILD)/test_graphsort` prints PASS with the new cases.
- `grep -c graph_sort_key cli/asmspy.c` unchanged (no TUI call-site edits).
- The header compiles under `g++ -std=c++17 -Icli -Iinclude` with T5's shim.

### T5 — vm_compat.cpp: the view-model reuse TU  (S, depends on: T2, T4)

**Goal.** One TU compiling the asmspy view-model headers as C++17 for both binaries, honoring the verified compile facts and include order.

**Steps.** Create `desktop/src/vm_compat.cpp` (Code — the comments are part of the deliverable) and add its object to all three trees (app, render, test): its compiling **is** the regression test keeping the headers C++-clean.

**Code.** Full initial content:

```cpp
/* vm_compat.cpp — compiles the asmspy view-model headers as C++17 so desktop
 * views reuse the TUI's tested inline logic (plan D5). Verified 2026-07-23:
 *  1. cli/asmspy.h includes <stdatomic.h> (:22); GCC provides no atomic_bool
 *     for C++ < 23, so the alias below must precede it. Name-level fix only,
 *     not an ABI claim: the desktop never links the engines (D9) and never
 *     defines an atomic_bool object.
 *  2. cli/asmspy_autoregion.h uses asmspy_sample_edge_t (cli/asmspy.h:509) in
 *     its signatures WITHOUT including asmspy.h — asmspy.h must come first
 *     (the plan-D5 include-order dependency, reproduced by compilation). */
#include <atomic>
using std::atomic_bool;

#include "asmspy.h"            /* FIRST: supplies autoregion/graphsort types */
#include "asmspy_logview.h"
#include "asmspy_dataview.h"
#include "asmspy_autoregion.h" /* AFTER asmspy.h (fact 2)                    */
#include "asmspy_graphsort.h"  /* AFTER T4 (context lift + C++ casts)        */
/* asmspy_treefilter.h arrives via asmspy.h:27; its guard makes that fine.    */

namespace asmdesk {
/* Touch one symbol per header: an empty include (broken guard, wrong -I)
 * fails HERE, not in a view; also keeps -Wunused-function quiet for the qsort
 * adapter, which the GUI must never call (single-thread latch). */
int vm_compat_anchor() {
    long top = 0;
    (void)asmspy_log_window(1, 1, 0, 1, &top);
    (void)gnode_cmp;
    asmspy_autocand_t c[1];
    return (int)asmspy_autoregion_rank(nullptr, 0, nullptr, nullptr, nullptr,
                                       c, 1);
}
} // namespace asmdesk
```

**Tests.** Compiling warning-clean in all three trees under T2's `-Wall -Wextra`; `make desktop-test` covers the test tree.

**Docs.** The TU's comment block.

**Done when.**
- `make desktop-test` and both app links build `vm_compat.o` warning-clean.
- Removing `using std::atomic_bool;` breaks the build (the shim is still load-bearing — the headers have not been silently forked).

### T6 — Window shell: main.cpp, three doors, open dialog, tab strip  (M, depends on: T2, T3)

**Goal.** The visible skeleton: one shared `main.cpp` for both binaries (compile-time `ASMTEST_DESKTOP_RENDER_ONLY` guard), home screen with the three doors as placeholders, recording-open dialog, tab strip over the Workspace — all drawable headlessly.

**Steps.**
1. Create `desktop/src/ui/shell.h`/`shell.cpp` (Code). Draw code is backend-free (pure ImGui immediate calls over `ShellState`) so the null backend drives it in tests.
2. Create `desktop/src/main.cpp`: GLFW init → window (title `asmtest desktop`, or `asmtest viewer (render-only)` under the guard) → GL3 + ImGui GLFW/GL3 backends → loop (`glfwPollEvents`, backend NewFrames, `ImGui::NewFrame`, `asmdesk::draw_shell(state)`, `ImGui::Render`, backend RenderDrawData, `glfwSwapBuffers`) → teardown. Stock ImGui GLFW+OpenGL3 example shape.
3. Doors (placeholders — behavior lands in [06-doors-and-learning.md](06-doors-and-learning.md) / [08-observer-views.md](08-observer-views.md)): **Learn** opens the open dialog; **Author**/**Inspect** render disabled in the render-only binary with the verbatim reason (greyed-out-shows-why law, plan D2): `requires the full app (GPL-2.0; links the engines) — this is the render-only viewer`. In the full app they open an empty tab titled by the door.
4. Open dialog: `InputText` path + Open/Cancel; `Workspace::open` errors render verbatim in the dialog (never a silent no-op).
5. Tab strip: one tab per `Recording` (title = filename); content here is the summary pane — per-kind event counts + provenance chrome (producer, backend, exact/statistical, drops) + the truncation banner via a pure helper so tests assert it without pixels.
6. Create `desktop/test/test_shell.cpp`: build a `ShellState`, open the T3 fixtures, run 3 null-backend frames of `draw_shell` (no crash), and assert `shell_banner` across fixtures: non-null for `truncated`/`dropped`/ `torn-tail`, null for `min-trace` (D7 as behavior).

**Code.**

```cpp
// desktop/src/ui/shell.h  (namespace asmdesk; new)
struct ShellState {
    Workspace ws;
    int active_tab = -1;          // -1 = home (the three doors)
    bool open_dialog = false;
    char open_path[1024] = {0};
    std::string open_error;
};
void draw_shell(ShellState &s);               // backend-free ImGui draws
const char *shell_banner(const Recording &r); // pure; nullptr when clean, else
                                              // e.g. "TRUNCATED recording — drops: N"
```

**Tests.** Step 6, wired into `desktop-test`.

**Docs.** Header comments only (docs/internal is Sphinx-excluded per [_conventions](../implementations/_conventions.md)).

**Done when.**
- `make desktop && ./build/asmtest-desktop` opens a window with three doors on a GL-capable host; `./build/asmtest-viewer` shows Author/Inspect disabled + reason label.
- `ldd build/asmtest-viewer` shows no libunicorn/libkeystone/libcapstone (D4).
- `make desktop-test` runs `test_shell` green headlessly, host and docker.

### T7 — Golden-recording smoke over tests/golden-asmtrace/  (M, depends on: T3, T6, 01's corpus)

**Goal.** `desktop-test` proves the viewer opens every committed golden recording: parse + model invariants + a 3-frame null render — the schema-stability gate on the consumer side (plan DX golden-recording tests).

**Steps.**
1. Create `desktop/test/test_golden.cpp`: iterate `tests/golden-asmtrace/*.asmtrace` (dir from a make-provided define; corpus committed by 01 per D6). Per file: `load_recording_file` must succeed; invariants: `version == 1`, `provenance.producer`/`backend` non-empty, event count > 0, sum of `by_kind` sizes == parsed event lines, and — for the corpus's dishonesty fixtures (D7) — `truncated() ⇒ shell_banner(r) != nullptr`. Then open all recordings into one `ShellState` and run the 3-frame null loop. An empty or missing dir is a **FAILURE**: the corpus is committed, absence is a broken checkout, and a test that can only self-skip is not a test ([CLAUDE.md](../../../CLAUDE.md)).
2. Add `test_golden` to the `desktop-test` recipe (after the other three) — it thereby runs inside `docker-desktop` with no extra wiring. **Land this task only after 01's corpus is committed**; never land it early behind a skip.
3. Byte-stability round-trips are 01's gate (D6); this asserts openability + invariants only — no golden regeneration here.

**Code.** None beyond the test file. **Tests.** This task is a test: a corpus regeneration that renames a field names the failing file; an ImGui upgrade that breaks a draw crashes the 3-frame loop in-lane, not on a user.

**Docs.** A comment block atop `test_golden.cpp` citing D6/D7 and 01.

**Done when.**
- `make desktop-test` (host and `make docker-desktop`) runs `test_golden` green over every committed golden file.
- Deleting `tests/golden-asmtrace/` makes it FAIL (not skip).

### T8 — desktop/README.md + changelog  (S, depends on: T1–T6)

**Goal.** The D1-required `desktop/README.md` and the changelog entry.

**Steps.**
1. Write `desktop/README.md`: the two binaries and the license split (full app GPL-2.0 as a whole via the bundled engines; render-only viewer permissive and engine-free — plan §D7 / D4); the four make targets + docker lane; the pinned deps (versions, fetch scripts, digest rows); layout (`src/doc/`, `src/ui/`, `test/`, `test/fixtures/`); pointer to this doc set.
2. Append one `### Added` bullet under `## [Unreleased]` in [CHANGELOG.md](../../../CHANGELOG.md#L7): desktop GUI skeleton (two binaries, `.asmtrace` loader, docker lane).

**Tests.** None (prose). **Docs.** This task is the docs.

**Done when.** `desktop/README.md` exists and states the license split explicitly; CHANGELOG has the one bullet.

## Task order & parallelism

- **T1** and **T4** are independent of everything and each other — start both immediately (disjoint files: `scripts/`+`licenses/` vs `cli/`).
- **T2** follows T1. **T3** follows T2 (json include path + test target); its model code is plain C++ and can be drafted alongside T2.
- **T5** needs T2 + T4. **T6** needs T2 + T3.
- **T7** is gated on 01's committed corpus — everything else proceeds on T3's hand-written fixtures without waiting. **T8** last.

Critical path: **T1 → T2 → T3 → T6 → T7**; off it: T4 → T5, and T8.

## Constraints & gates

- **Sibling contracts.** The `.asmtrace` grammar, event-kind registry, state descriptors, and `make asmtrace-golden` belong to [01-asmtrace-format.md](01-asmtrace-format.md) (draft schema at [asmtrace-schema.md](asmtrace-schema.md) until the Phase-3 freeze, D5); record/serve producers to [02-exporters-and-readers.md](02-exporters-and-readers.md) / [07-serve-live-host.md](07-serve-live-host.md).
- **No self-skip anywhere here.** GLFW/GL are installable (apt), ImGui/json fetchable — per [CLAUDE.md](../../../CLAUDE.md) they go into `Dockerfile.desktop`, and `desktop-test` needs no display or GL at all. No hardware or credential gate exists in this doc's scope.
- **CI follow-up gate (recorded, deliberately not done here).** Once T7 lands, a `docker-desktop` job belongs in `.github/workflows/ci.yml` beside the existing docker lanes, mirroring how `docker-cli` runs its smoke. This doc does **not** edit ci.yml; the CI owner adds the job only.
- **Licensing (D4/D9).** The full app links bundled Unicorn/Keystone → GPL-2.0 as a whole; `asmtest-viewer` must never grow an engine object or lib (T6's `ldd` check is the gate). Pin/SDE/libdft64 never appear in any desktop artifact. The desktop never links the asmspy ptrace engines — Observer capture is the `asmspy --serve` subprocess (D9, owned by 07).
- **Digest-manifest drift.** Never run [refresh-thirdparty-digests.sh](../../../scripts/refresh-thirdparty-digests.sh) after T1 — it rewrites the manifest in full and does not know the hand-added rows (T1's included). Extending it is a whole-manifest fix this doc records but does not own.
- **Formatting (D8).** desktop/ C++ uses the repo [.clang-format](../../../.clang-format) (`Language: Cpp`); `desktop-fmt-check` is informational; desktop/ stays out of the CI-gated [FMT_SOURCES](../../../Makefile#L776).
- **Threading.** GUI panels call `gnode_cmp_key` (T4), never the `graph_sort_key` latch. No tracer thread exists in this doc's scope (that arrives with 07/08 under the plan-D5 one-tracer-thread rule).

## Out of scope

- **Any real view** (slice explorer, timelines, canvas, diff UI, the Loom) — docs [04](04-replay-views.md)/[05](05-loom-day-one.md); **door behavior** — docs 06/08; **live capture / `--serve`** — 07; **record modes, exporters, schema/serializers** — 01/02; **teaching producers** — [09](09-teaching-producers.md).
- **Windows/macOS packaging, installers, signing** — the plan's distribution-honesty stance (build-from-source + docker lane) stands for v1.
- **A file-picker dependency** — the open dialog is a text field until a view doc needs more.
- **Editing `.github/workflows/ci.yml` or `hw.yml`** — recorded above as the follow-up gate.
- **The asmspy TUI and engines** — untouched except T4's surgical cli/ edits, whose TUI behavior `test_graphsort` pins unchanged.
