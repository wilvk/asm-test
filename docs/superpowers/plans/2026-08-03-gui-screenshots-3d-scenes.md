# GUI Screenshots and 3D Scenes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce a committed set of documentation screenshots showing every 3D scene kind populated with real data, a repeatable way to regenerate them, and a sample process whose shape can fill those scenes.

**Architecture:** A new `cli/scenes_victim.c` sample process is shaped so that four live `asmspy` attaches produce recordings satisfying all five scene availability gates. A new `--shot` mode in `asmtest-desktop` renders the real ImGui shell into a surfaceless-EGL framebuffer and writes PNGs, driven by a JSON manifest. A Sphinx guide page carries the images and explains how to reproduce them.

**Tech Stack:** C11 (victim), C++17 + Dear ImGui 1.91.9b-docking + OpenGL 3.3 + EGL (shot mode), nlohmann/json (manifest), stb_image_write (PNG), GNU Make, Sphinx/MyST (docs).

**Spec:** `docs/superpowers/specs/2026-08-03-gui-screenshots-3d-scenes-design.md`

## Global Constraints

- **`--shot` lands ONLY in `asmtest-desktop`.** `build/asmtest-viewer` is the permissive render-only binary and its link line must gain **no** new object or library. Guard all shot code with `#ifndef ASMTEST_DESKTOP_RENDER_ONLY`. Verify with `ldd build/asmtest-viewer`.
- **Four separate recordings are structural, not a convenience.** `call` events and `df_*` events come from different engines; `--serve` runs one engine at a time; `Session::done_` is a vector of separate `Recording`s. Do not try to merge them.
- **Divergence A/B must differ in DATA only, never in compiled code.** `build_divergence_scene` gates on matching `code_sha`/basis/arch and emits a refusal card on mismatch. The variant is a runtime `--seed`, never a `#define`.
- **`df_step` operands live under the JSON key `ops`, not `vals`.** Reading `vals` silently yields zero records and looks exactly like a producer gap.
- **`--fpregs` is NOT required for LanePrism.** Wide XMM records come from any `--dataflow` attach on SSE code. `--fpregs` adds the XMM deck to `regstate`, which the Scrubber reads.
- **`cli/*.c` is inside the CI-gated `make fmt-check`.** `desktop/` is not (it has its own ungated `desktop-fmt-check`). Run `make fmt-check` after touching `cli/`.
- **Shared tree.** Many agents push to `main` live. Commit only your own paths using a private `GIT_INDEX_FILE`, then repair the shared index with a path-scoped `git reset -- <paths>`. Push after every commit.
- **Third-party deps are pinned.** A new dependency needs a `scripts/fetch-*.sh` plus a row in `scripts/third-party-digests.txt`, following `scripts/fetch-linmath.sh` exactly.
- Image size for every shot: **1600×1000**. Warmup before capture: **30 frames**.

---

### Task 1: The sample process — `cli/scenes_victim.c`

**Files:**
- Create: `cli/scenes_victim.c`
- Modify: `mk/cli.mk` (add the build rule next to `auto_victim`'s, ~line 241; add to the `cli-smoke` prerequisite list, ~line 633)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: a binary at `build/scenes_victim` exporting these **exact** symbol names, which Task 2 attaches to by name:
  - `blend_tile` — the SSE hot routine (`--dataflow` and `--trace` target)
  - `walk_heap`, `sort_batch`, `mix_math` — the worker-thread call-tree spine
  - CLI: `--seed <n>` (default 1), `--threads <n>` (default 3)
  - stderr line 1: `scenes_victim pid=<pid> blend_tile=<addr> seed=<n>`

- [ ] **Step 1: Write the failing test**

Create `cli/test_scenes_victim.sh`:

```sh
#!/bin/sh
# test_scenes_victim.sh — the sample process's SHAPE is the contract. Each check
# below maps to one 3D scene gate in desktop/src/ui/shell.cpp.
set -eu
BIN="${1:-build/scenes_victim}"

fail() { echo "FAIL: $*" >&2; exit 1; }

[ -x "$BIN" ] || fail "$BIN not built"

# It must announce pid + the routine address on stderr, so a capture script can
# read them without guessing.
out=$("$BIN" --selftest 2>&1) || fail "--selftest exited nonzero"
echo "$out" | grep -q "^scenes_victim pid=[0-9]* blend_tile=0x[0-9a-f]* seed=1$" \
    || fail "stderr banner missing or malformed: $out"

# Same seed must be deterministic; a different seed must diverge. This is what
# makes the Divergence scene possible at all.
a=$("$BIN" --selftest --seed 1 2>/dev/null)
b=$("$BIN" --selftest --seed 1 2>/dev/null)
c=$("$BIN" --selftest --seed 2 2>/dev/null)
[ "$a" = "$b" ] || fail "same seed produced different output (not deterministic)"
[ "$a" != "$c" ] || fail "seed 1 and seed 2 produced identical output (no divergence)"

# Every symbol the capture step names must be a real, resolvable ELF symbol.
for sym in blend_tile walk_heap sort_batch mix_math; do
    nm "$BIN" 2>/dev/null | grep -q " [Tt] $sym$" || fail "missing ELF symbol: $sym"
done

# blend_tile must actually contain SSE writes, or the LanePrism scene is empty.
objdump -d --disassemble="blend_tile" "$BIN" 2>/dev/null \
    | grep -qE "paddd|pshufd|punpck" \
    || fail "blend_tile contains no recognisable SSE lane ops"

echo "PASS: scenes_victim shape"
```

Make it executable: `chmod +x cli/test_scenes_victim.sh`

- [ ] **Step 2: Run it to verify it fails**

Run: `sh cli/test_scenes_victim.sh`
Expected: `FAIL: build/scenes_victim not built`

- [ ] **Step 3: Write the sample process**

Create `cli/scenes_victim.c`:

```c
/* scenes_victim.c — the documented sample process for the desktop GUI's 3D
 * scenes (docs/guides/desktop-gui-scenes.md).
 *
 * THE SHAPE IS THE REQUIREMENT. Every element below exists to satisfy exactly
 * one scene availability gate in desktop/src/ui/shell.cpp; drop any one of them
 * and the corresponding scene renders an "unavailable" card instead of geometry.
 *
 *   blend_tile()   SSE work on a 16-byte tile. The dataflow producer decodes its
 *                  XMM operands into WIDE value records (>8 bytes, carried in
 *                  the `bytes` field), which is the LanePrism scene's only
 *                  input. The mnemonics are chosen so lane_width_for() can name
 *                  the element width from the disassembly — a mnemonic it
 *                  cannot classify degrades every write to a default lane width.
 *                  Its ENTRY is also arrived at constantly, which is what an
 *                  --trace region capture needs: a routine entered once yields
 *                  one invocation, and one invocation is not a scene.
 *
 *   worker()       Three threads descending through walk_heap/sort_batch/mix_math
 *                  into libc and libm. The ModuleRibbon scene's lanes are tids,
 *                  its Y is call depth and its colour is the module — so it needs
 *                  more than one thread AND more than one library, or it
 *                  degenerates into a single stripe.
 *
 *   walk_heap()    A strided walk over a few hundred KB. The address plane needs
 *                  observed data spans to have terrain at all, and the data-cell
 *                  and relief layers read the resolved effective addresses that
 *                  `asmspy --dataflow --mem` emits.
 *
 *   --seed N       Changes INPUT DATA ONLY, never code. The Divergence scene
 *                  gates on a matching code_sha/basis/arch and only then diffs
 *                  the statediff streams; a variant that changed a compiled
 *                  constant would produce a refusal card rather than a fork.
 *                  Two runs at different seeds share an identical prefix and
 *                  then diverge, which is exactly the scene's subject.
 *
 * blend_tile carries its own -O2 attribute: at the file's default -O0 the SSE
 * intrinsics expand into ~56 steps of movdqa traffic through stack slots, which
 * renders as a cluttered prism. The rest of the file stays at the tree's flags.
 *
 * Opts in via PR_SET_PTRACER_ANY like every other victim here: Yama
 * ptrace_scope=1 is the Ubuntu default and is NOT namespaced, so without it the
 * sibling attach is denied and the failure reads like a tracer bug.
 */
#include <emmintrin.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>

#ifndef PR_SET_PTRACER
#define PR_SET_PTRACER 0x59616d61
#endif
#ifndef PR_SET_PTRACER_ANY
#define PR_SET_PTRACER_ANY ((unsigned long)-1)
#endif

#define HEAP_CELLS 32768 /* 256 KB of long — the terrain's data spans */
#define HEAP_STRIDE 97   /* coprime with HEAP_CELLS: a spread, not a sweep */
#define SORT_N 64

static volatile long g_sink;
static long *g_heap;
static int g_tile[4] __attribute__((aligned(16)));

/* The SSE hot routine: the LanePrism scene's whole input, and the region an
 * --trace capture pages by invocation. -O2 locally so the captured window is
 * tight enough to read; see the file comment. */
__attribute__((noinline, optimize("O2"))) long blend_tile(long x) {
    __m128i a = _mm_loadu_si128((const __m128i *)g_tile);
    __m128i b = _mm_set1_epi32((int)(x & 0x7fffffff));
    __m128i c = _mm_add_epi32(a, b);      /* paddd  — 4-byte lanes, nameable */
    c = _mm_shuffle_epi32(c, 0x1B);       /* pshufd — reverses the lane order */
    c = _mm_unpacklo_epi32(c, a);         /* punpckldq — interleaves two writes */
    _mm_storeu_si128((__m128i *)g_tile, c);
    return (long)_mm_cvtsi128_si32(c);
}

/* A strided walk: the terrain's observed data spans and the data-cell layers. */
__attribute__((noinline)) long walk_heap(long x) {
    long acc = 0;
    for (int i = 0; i < 256; i++) {
        size_t idx = (size_t)((x * HEAP_STRIDE + i * HEAP_STRIDE) % HEAP_CELLS);
        g_heap[idx] += x + i;
        acc += g_heap[idx];
    }
    return acc;
}

static int cmp_long(const void *a, const void *b) {
    long x = *(const long *)a, y = *(const long *)b;
    return (x > y) - (x < y);
}

/* Calls into libc (qsort, memcpy): a second module in the ribbon. */
__attribute__((noinline)) long sort_batch(long x) {
    long buf[SORT_N], tmp[SORT_N];
    for (int i = 0; i < SORT_N; i++)
        buf[i] = (x * 2654435761u + (unsigned)i * 40503u) & 0xffff;
    qsort(buf, SORT_N, sizeof(buf[0]), cmp_long);
    memcpy(tmp, buf, sizeof(buf));
    return tmp[0] + tmp[SORT_N - 1];
}

/* Calls into libm (sin/sqrt): a third module, at a deeper call depth. */
__attribute__((noinline)) long mix_math(long x) {
    double d = sin((double)(x & 0xff) * 0.017453292519943295);
    return (long)(sqrt(fabs(d) + 1.0) * 1000.0);
}

struct worker_arg {
    long seed;
    int depth; /* which helpers this thread descends through */
};

static void *worker(void *p) {
    struct worker_arg *w = (struct worker_arg *)p;
    for (long i = 0;; i++) {
        long x = w->seed + i;
        g_sink += blend_tile(x);
        if (w->depth >= 1)
            g_sink += walk_heap(x);
        if (w->depth >= 2)
            g_sink += sort_batch(x);
        if (w->depth >= 3)
            g_sink += mix_math(x);
        if ((i & 0x3ff) == 0)
            sched_yield(); /* be a good citizen on a shared box */
    }
    return NULL;
}

/* One deterministic pass, printed. --selftest runs this and exits, so the shape
 * test can assert determinism and divergence without attaching a tracer. */
static void selftest(long seed) {
    long acc = 0;
    for (long i = 0; i < 32; i++) {
        long x = seed + i;
        acc += blend_tile(x) + walk_heap(x) + sort_batch(x) + mix_math(x);
    }
    printf("selftest seed=%ld acc=%ld\n", seed, acc);
}

int main(int argc, char **argv) {
    long seed = 1;
    int nthreads = 3, self = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc)
            seed = strtol(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc)
            nthreads = (int)strtol(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--selftest") == 0)
            self = 1;
        else {
            fprintf(stderr, "usage: %s [--seed N] [--threads N] [--selftest]\n",
                    argv[0]);
            return 2;
        }
    }
    if (nthreads < 1)
        nthreads = 1;
    if (nthreads > 8)
        nthreads = 8;

    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);

    g_heap = (long *)calloc(HEAP_CELLS, sizeof(long));
    if (g_heap == NULL) {
        fprintf(stderr, "scenes_victim: out of memory\n");
        return 1;
    }
    for (int i = 0; i < 4; i++)
        g_tile[i] = i + 1;

    fprintf(stderr, "scenes_victim pid=%d blend_tile=%p seed=%ld\n", (int)getpid(),
            (void *)blend_tile, seed);
    fflush(stderr);

    if (self) {
        selftest(seed);
        free(g_heap);
        return 0;
    }

    pthread_t th[8];
    struct worker_arg args[8];
    for (int i = 0; i < nthreads; i++) {
        args[i].seed = seed + i * 1000;
        args[i].depth = i + 1; /* distinct depths => distinct ribbon lanes */
        if (pthread_create(&th[i], NULL, worker, &args[i]) != 0) {
            fprintf(stderr, "scenes_victim: pthread_create failed\n");
            return 1;
        }
    }
    for (int i = 0; i < nthreads; i++)
        pthread_join(th[i], NULL);
    free(g_heap);
    return 0;
}
```

- [ ] **Step 4: Add the build rule**

In `mk/cli.mk`, immediately after the `$(BUILD)/auto_victim` rule (~line 242), add:

```make
# scenes_victim backs the documented 3D-scene screenshots
# (docs/guides/desktop-gui-scenes.md) and its SHAPE is the requirement: SSE lane
# writes for the LanePrism scene, a constantly-entered routine for the Invocation
# scene, threads across libc/libm for the ModuleRibbon, a strided heap walk for
# the terrain's data spans, and a --seed that changes DATA ONLY so two runs share
# a code_sha and can diverge. Needs -lpthread and -lm for the module spine.
$(BUILD)/scenes_victim: $(BUILD)/scenes_victim.o
	$(CC) $(CFLAGS) $^ -lpthread -lm -o $@
```

In the `cli-smoke` prerequisite list (~line 636), add `$(BUILD)/scenes_victim` to the line containing `$(BUILD)/auto_victim`:

```make
           $(BUILD)/auto_victim $(BUILD)/quiet_hot_victim $(BUILD)/scenes_victim \
```

- [ ] **Step 5: Build and run the test**

Run: `make build/scenes_victim && sh cli/test_scenes_victim.sh`
Expected: `PASS: scenes_victim shape`

If `objdump --disassemble=` is unavailable on the host, the SSE check falls back
to scanning the whole binary — do not weaken the check; install binutils.

- [ ] **Step 6: Verify the format gate**

Run: `make fmt-check`
Expected: no drift reported. If it reports drift in `cli/scenes_victim.c`, run
`make fmt` and re-run `make fmt-check`.

- [ ] **Step 7: Commit and push**

```bash
S=$(mktemp -d)
cat > $S/msg <<'EOF'
cli: add scenes_victim, the sample process for the 3D scene docs

Its shape is the requirement: SSE lane writes feed the LanePrism scene, a
constantly-entered blend_tile feeds an --trace region capture (a routine entered
once yields one invocation, which is not a scene), three threads across libc and
libm feed the ModuleRibbon's tid lanes and module colours, and a strided heap
walk gives the address plane observed data spans.

--seed changes input DATA only, never compiled code: the Divergence scene gates
on a matching code_sha and would show a refusal card rather than a fork if the
two sides differed in their bytes.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
export GIT_INDEX_FILE=$S/idx; git read-tree HEAD
git add cli/scenes_victim.c cli/test_scenes_victim.sh mk/cli.mk
T=$(git write-tree); O=$(git rev-parse HEAD)
N=$(git commit-tree $T -p $O -F $S/msg)
git update-ref refs/heads/main $N $O && unset GIT_INDEX_FILE && git push origin main
git reset -q -- cli/scenes_victim.c cli/test_scenes_victim.sh mk/cli.mk
git status --short   # MUST be clean
```

---

### Task 2: The recordings target

**Files:**
- Create: `scripts/capture-shot-recordings.sh`
- Modify: `mk/desktop.mk` (add `gui-shot-recordings` target and its `.PHONY` entry at ~line 894)

**Interfaces:**
- Consumes: `build/scenes_victim` from Task 1, with symbols `blend_tile`, `walk_heap`, `sort_batch`, `mix_math`.
- Produces: four recordings that Task 6's manifest names by path:
  - `build/shots/rec/tree.asmtrace` — carries `call` events (ModuleRibbon)
  - `build/shots/rec/trace-blend.asmtrace` — carries `trace` + `coverage` (Invocation)
  - `build/shots/rec/df-a.asmtrace` — carries `df_step` with wide `ops`, `mem`, `regstate`, `statediff` (Plane, LanePrism, Divergence A, Scrubber)
  - `build/shots/rec/df-b.asmtrace` — same kinds, seed 2 (Divergence B)

- [ ] **Step 1: Write the failing test**

Create `scripts/verify-shot-recordings.py`:

```python
#!/usr/bin/env python3
"""verify-shot-recordings.py — assert each capture carries the event kinds its
scene needs, so a producer regression surfaces here rather than as an empty
scene in a committed screenshot.

NOTE: df_step operands live under the key "ops". "vals" does not exist; reading
it yields zero records and looks exactly like a producer gap."""
import json
import sys

REC_DIR = sys.argv[1] if len(sys.argv) > 1 else "build/shots/rec"

def load(name):
    kinds, wide_regs = {}, set()
    with open(f"{REC_DIR}/{name}", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                o = json.loads(line)
            except json.JSONDecodeError:
                continue
            if not isinstance(o, dict):
                continue
            k = o.get("k", "?")
            kinds[k] = kinds.get(k, 0) + 1
            if k == "df_step":
                for v in o.get("ops") or []:
                    if v.get("wide") and v.get("space") == "reg":
                        wide_regs.add(v["reg"])
    return kinds, wide_regs

failures = []

def need(name, kinds, kind, atleast, why):
    got = kinds.get(kind, 0)
    if got < atleast:
        failures.append(f"{name}: {kind} count {got} < {atleast} — {why}")

k, _ = load("tree.asmtrace")
need("tree.asmtrace", k, "call", 50, "ModuleRibbon has no call tree")

k, _ = load("trace-blend.asmtrace")
need("trace-blend.asmtrace", k, "trace", 100, "Invocation has no instructions")
need("trace-blend.asmtrace", k, "coverage", 4,
     "a coverage event CLOSES an invocation; <4 gives too few slabs")

for side in ("df-a.asmtrace", "df-b.asmtrace"):
    k, wide = load(side)
    need(side, k, "df_step", 20, "Plane/LanePrism have no dataflow")
    need(side, k, "mem", 10, "the data-cell layers have no addresses")
    need(side, k, "statediff", 1, "Divergence cannot diff without statediff")
    if not wide:
        failures.append(
            f"{side}: no wide reg records — LanePrism would be empty. "
            "Check that blend_tile still compiles to SSE.")

if failures:
    print("FAIL:")
    for f in failures:
        print("  " + f)
    sys.exit(1)
print("PASS: all four recordings carry the kinds their scenes need")
```

- [ ] **Step 2: Run it to verify it fails**

Run: `python3 scripts/verify-shot-recordings.py`
Expected: `FileNotFoundError` for `build/shots/rec/tree.asmtrace`

- [ ] **Step 3: Write the capture script**

Create `scripts/capture-shot-recordings.sh`:

```sh
#!/bin/sh
# capture-shot-recordings.sh — capture the four recordings the documented 3D
# scene screenshots are rendered from.
#
# FOUR recordings, not one, and this is structural: `call` events and `df_*`
# events come from different engines, --serve runs ONE engine at a time, and the
# desktop's Session::done_ is a vector of separate Recordings. No single capture
# can satisfy every scene gate.
#
# Needs to attach to a process it did not launch. scenes_victim calls
# PR_SET_PTRACER_ANY so this works under the Ubuntu default ptrace_scope=1; see
# docs/getting-started/host-setup.md if attach is denied anyway.
set -eu

ASMSPY="${ASMSPY:-build/asmspy}"
VICTIM="${VICTIM:-build/scenes_victim}"
OUT="${OUT:-build/shots/rec}"

[ -x "$ASMSPY" ] || { echo "capture: $ASMSPY not built (make cli)" >&2; exit 1; }
[ -x "$VICTIM" ] || { echo "capture: $VICTIM not built" >&2; exit 1; }

mkdir -p "$OUT"

pids=""
cleanup() { for p in $pids; do kill "$p" 2>/dev/null || true; done; }
trap cleanup EXIT INT TERM

start_victim() { # $1 = seed -> echoes pid
    "$VICTIM" --seed "$1" 2>/dev/null &
    p=$!
    pids="$pids $p"
    sleep 1                      # let the workers reach steady state
    kill -0 "$p" 2>/dev/null || { echo "capture: victim died" >&2; exit 1; }
    echo "$p"
}

say() { echo "capture: $*" >&2; }

# --- ModuleRibbon: the whole-process call tree across threads and modules -----
v1=$(start_victim 1)
say "tree      <- pid $v1"
"$ASMSPY" --tree "$v1" 400 --record="$OUT/tree.asmtrace" >/dev/null 2>&1 || true

# --- Invocation: N coverage events == N invocation slabs ---------------------
say "trace     <- pid $v1 blend_tile"
"$ASMSPY" --trace "$v1" blend_tile 12 --record="$OUT/trace-blend.asmtrace" \
    >/dev/null 2>&1 || true

# --- Plane / LanePrism / Divergence A ----------------------------------------
# --fpregs is here for the SCRUBBER's wide register deck. LanePrism does not
# need it: the dataflow producer reads XMM operands directly.
say "df-a      <- pid $v1 blend_tile"
"$ASMSPY" --dataflow "$v1" blend_tile --steps --mem --fpregs --statediff \
    --record="$OUT/df-a.asmtrace" >/dev/null 2>&1 || true
kill "$v1" 2>/dev/null || true

# --- Divergence B: same code, different DATA ---------------------------------
v2=$(start_victim 2)
say "df-b      <- pid $v2 blend_tile (seed 2)"
"$ASMSPY" --dataflow "$v2" blend_tile --steps --mem --fpregs --statediff \
    --record="$OUT/df-b.asmtrace" >/dev/null 2>&1 || true
kill "$v2" 2>/dev/null || true

say "wrote $OUT/{tree,trace-blend,df-a,df-b}.asmtrace"
```

Make it executable: `chmod +x scripts/capture-shot-recordings.sh scripts/verify-shot-recordings.py`

- [ ] **Step 4: Add the make target**

In `mk/desktop.mk`, add to the `.PHONY` line at ~894 the two names `gui-shot-recordings gui-shots`, then add this target near the other `gui-*` rules:

```make
# The four recordings the documented 3D-scene screenshots render from. Separate
# from gui-shots so a re-render does not re-attach to a live process.
gui-shot-recordings: $(BUILD)/asmspy $(BUILD)/scenes_victim
	sh scripts/capture-shot-recordings.sh
	python3 scripts/verify-shot-recordings.py
```

- [ ] **Step 5: Run the capture and the verifier**

Run: `make gui-shot-recordings`
Expected: `PASS: all four recordings carry the kinds their scenes need`

If a `need()` check fails, the fix is in `cli/scenes_victim.c`'s shape or in the
asmspy flags in the capture script — **not** in the thresholds. Lowering a
threshold to make it pass defeats the purpose of the check.

- [ ] **Step 6: Commit and push**

Use the Task 1 Step 7 command block, substituting the paths
`scripts/capture-shot-recordings.sh scripts/verify-shot-recordings.py mk/desktop.mk`
and this message subject: `build: capture and verify the 3D-scene screenshot recordings`.

---

### Task 3: Pinned stb_image_write and the PNG writer

**Files:**
- Create: `scripts/fetch-stb.sh`
- Modify: `scripts/third-party-digests.txt`
- Create: `desktop/src/ui/png_write.h`, `desktop/src/ui/png_write.cpp`
- Create: `desktop/test/test_png_write.cpp`
- Modify: `mk/desktop.mk` (fetch wiring; test binary rule; `DESKTOP_TESTS` list at ~line 1127)

**Interfaces:**
- Produces, used by Task 5:
  ```cpp
  namespace asmdesk {
  // Write an RGBA8 buffer as a PNG. `px` must hold w*h*4 bytes in the row order
  // glReadPixels produces (bottom-up); this flips to top-down for the file.
  // Returns false with `err` set on any IO or encode failure.
  bool png_write_rgba_flipped(const std::string &path, int w, int h,
                              const unsigned char *px, std::string &err);
  }
  ```

- [ ] **Step 1: Write the failing test**

Create `desktop/test/test_png_write.cpp`:

```cpp
// test_png_write.cpp — the shot mode's PNG writer. A committed screenshot that
// silently encoded to garbage would look exactly like a rendering bug, so the
// header and dimensions are asserted rather than assumed.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "ui/png_write.h"

static int g_fail = 0;
static void check(const char *what, bool ok, const char *why) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        std::printf("     %s\n", why);
        g_fail = 1;
    }
}

static std::vector<unsigned char> read_all(const std::string &p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(f)),
                                      std::istreambuf_iterator<char>());
}

int main() {
    const std::string path = "build/test_png_write_out.png";
    std::remove(path.c_str());

    const int W = 8, H = 4;
    std::vector<unsigned char> px(static_cast<size_t>(W) * H * 4, 0);
    // Bottom row (first in glReadPixels order) red; top row blue. After the
    // flip the FILE's first row must be the blue one.
    for (int x = 0; x < W; x++) {
        px[static_cast<size_t>(x) * 4 + 0] = 255; // bottom row red
        px[static_cast<size_t>(x) * 4 + 3] = 255;
        size_t top = (static_cast<size_t>(H - 1) * W + x) * 4;
        px[top + 2] = 255; // top row blue
        px[top + 3] = 255;
    }

    std::string err;
    bool ok = asmdesk::png_write_rgba_flipped(path, W, H, px.data(), err);
    check("png_write: writes without error", ok, err.c_str());

    std::vector<unsigned char> bytes = read_all(path);
    check("png_write: file is non-empty", bytes.size() > 64,
          "an empty or tiny file means the encoder silently failed");

    const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    bool sig_ok = bytes.size() >= 8;
    for (int i = 0; sig_ok && i < 8; i++)
        sig_ok = bytes[static_cast<size_t>(i)] == sig[i];
    check("png_write: PNG signature", sig_ok, "not a PNG file");

    // IHDR width/height are big-endian u32 at offsets 16 and 20.
    bool dim_ok = false;
    if (bytes.size() >= 24) {
        unsigned w = (unsigned)bytes[16] << 24 | (unsigned)bytes[17] << 16 |
                     (unsigned)bytes[18] << 8 | (unsigned)bytes[19];
        unsigned h = (unsigned)bytes[20] << 24 | (unsigned)bytes[21] << 16 |
                     (unsigned)bytes[22] << 8 | (unsigned)bytes[23];
        dim_ok = (w == (unsigned)W && h == (unsigned)H);
    }
    check("png_write: IHDR carries the requested dimensions", dim_ok,
          "wrong width/height in the header");

    std::string err2;
    bool refused = !asmdesk::png_write_rgba_flipped(
        "build/no-such-dir-here/x.png", W, H, px.data(), err2);
    check("png_write: an unwritable path fails loudly", refused && !err2.empty(),
          "a failed write must set err, never return true silently");

    std::printf("%s\n", g_fail ? "test_png_write: FAILURES" : "test_png_write: all ok");
    return g_fail;
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `make build/desktop_test_png_write`
Expected: build failure — `ui/png_write.h: No such file or directory`

- [ ] **Step 3: Add the pinned fetch script**

Create `scripts/fetch-stb.sh` modelled exactly on `scripts/fetch-linmath.sh`:

```sh
#!/bin/sh
# fetch-stb.sh — fetch the pinned stb_image_write.h single header so the desktop
# GUI's --shot mode can encode PNGs without a system-wide install. Mirrors
# fetch-linmath.sh (same single-header, pinned-commit, hash-directly shape).
#
# Drops the header at build/stb/<ver>/stb_image_write.h so `#include
# "stb_image_write.h"` resolves with one -I. Prints STB_HOME on stdout:
#     STB_HOME=$(scripts/fetch-stb.sh)
# Idempotent: a present header is reused. Any OS.
#
# stb is dual-licensed MIT / public domain; licenses/stb-LICENSE.txt carries the
# verbatim notice from the pinned commit, exactly as fetch-linmath.sh does.
#
# Override STB_VERSION / STB_COMMIT / STB_URL to bump; STB_CACHE to relocate. On
# a bump: set the new commit, run this, copy the printed "got" digest into
# scripts/third-party-digests.txt (name "stb-image-write").
set -eu

STB_VERSION="${STB_VERSION:-f75e8d1}"
STB_COMMIT="${STB_COMMIT:-f75e8d1cad7d90d72ef7a4661f1b994ef78b4e31}"
STB_URL="${STB_URL:-https://raw.githubusercontent.com/nothings/stb/${STB_COMMIT}/stb_image_write.h}"
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib-thirdparty.sh"
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
STB_CACHE="${STB_CACHE:-$root/build/stb}"
home="$STB_CACHE/$STB_VERSION"
hdr="$home/stb_image_write.h"

log() { echo "fetch-stb: $*" >&2; }

if [ ! -e "$hdr" ]; then
    log "fetching stb_image_write.h @ $STB_COMMIT"
    mkdir -p "$home"
    tmp="$home/.stb_image_write.h"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL "$STB_URL" -o "$tmp"
    else
        wget -qO "$tmp" "$STB_URL"
    fi
    want=$(tp_digest tarball-sha256 stb-image-write "$STB_VERSION") || {
        log "ERROR: no pinned digest for stb-image-write $STB_VERSION in $TP_MANIFEST"
        log "       (add one by hand — refusing to use an unpinned download)"
        rm -f "$tmp"; exit 1
    }
    got="sha256:$(tp_sha256 "$tmp")"
    if [ "$got" != "$want" ]; then
        log "ERROR: stb_image_write.h $STB_VERSION integrity check FAILED"
        log "       expected $want"
        log "       got      $got"
        rm -f "$tmp"; exit 1
    fi
    log "verified stb_image_write.h $STB_VERSION ($got)"
    mv "$tmp" "$hdr"
    log "installed $hdr"
else
    log "reusing cached $hdr"
fi

[ -f "$hdr" ] || { log "ERROR: stb_image_write.h missing under $home"; exit 1; }
echo "$home"
```

`chmod +x scripts/fetch-stb.sh`

**Determine the real digest** (the placeholder above will fail the pin, which is
the correct behaviour):

```bash
curl -fsSL https://raw.githubusercontent.com/nothings/stb/f75e8d1cad7d90d72ef7a4661f1b994ef78b4e31/stb_image_write.h | sha256sum
```

If that commit no longer resolves, pick the current `master` commit of
`nothings/stb`, set `STB_COMMIT` to its full sha and `STB_VERSION` to its first 7
characters, and hash that.

Then append to `scripts/third-party-digests.txt`, next to the `linmath` row:

```
# stb_image_write.h: the desktop GUI's --shot PNG encoder. Compiled INTO
# asmtest-desktop (never the permissive viewer), so it is pinned like linmath.h.
# stb ships no tagged releases, so the pin is a commit; fetch-stb.sh resolves the
# full commit for the raw-file URL and hashes the header directly.
tarball-sha256  stb-image-write  <ver>  sha256:<digest from the command above>
```

Also save the upstream licence text to `licenses/stb-LICENSE.txt` (the LICENSE
file at the same pinned commit).

- [ ] **Step 4: Write the PNG writer**

Create `desktop/src/ui/png_write.h`:

```cpp
// png_write.h — PNG output for the --shot screenshot mode (gui screenshots
// spec, Component 2).
//
// Lives in the FULL binary only. asmtest-viewer is the permissive render-only
// build and must gain no new third-party object, so mk/desktop.mk links this
// into asmtest-desktop alone and every call site is guarded by
// !ASMTEST_DESKTOP_RENDER_ONLY.
#ifndef ASMDESK_UI_PNG_WRITE_H
#define ASMDESK_UI_PNG_WRITE_H

#include <string>

namespace asmdesk {

// Write an RGBA8 buffer as a PNG at `path`.
//
// `px` holds w*h*4 bytes in the order glReadPixels produces: the FIRST row in
// memory is the BOTTOM row of the image. This function flips to the top-down
// order a PNG file wants, so callers hand it the read-back buffer unmodified.
//
// Returns false with `err` set on any failure — an unwritable path, a bad size,
// or an encoder refusal. It never returns true without having written a file.
bool png_write_rgba_flipped(const std::string &path, int w, int h,
                            const unsigned char *px, std::string &err);

} // namespace asmdesk
#endif // ASMDESK_UI_PNG_WRITE_H
```

Create `desktop/src/ui/png_write.cpp`:

```cpp
#include "ui/png_write.h"

#include <cerrno>
#include <cstring>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO 0
#include "stb_image_write.h"

namespace asmdesk {

bool png_write_rgba_flipped(const std::string &path, int w, int h,
                            const unsigned char *px, std::string &err) {
    err.clear();
    if (w <= 0 || h <= 0 || px == nullptr) {
        err = "png_write: bad dimensions or null pixels";
        return false;
    }

    // glReadPixels gives bottom-up rows; a PNG is top-down. Flip by whole rows
    // rather than asking stb to do it via a global, so this stays thread-safe
    // and independent of stbi_flip_vertically_on_write's process-wide state.
    const size_t stride = static_cast<size_t>(w) * 4;
    std::vector<unsigned char> flipped(stride * static_cast<size_t>(h));
    for (int y = 0; y < h; y++)
        std::memcpy(&flipped[static_cast<size_t>(y) * stride],
                    px + static_cast<size_t>(h - 1 - y) * stride, stride);

    if (stbi_write_png(path.c_str(), w, h, 4, flipped.data(),
                       static_cast<int>(stride)) == 0) {
        err = "png_write: stbi_write_png failed for \"" + path + "\"";
        if (errno != 0)
            err += std::string(" (") + std::strerror(errno) + ")";
        return false;
    }
    return true;
}

} // namespace asmdesk
```

- [ ] **Step 5: Wire the build**

In `mk/desktop.mk`, near the other fetched-dependency blocks (the `LINMATH_HOME`
one is the model), add:

```make
# stb_image_write.h for the --shot PNG encoder. FULL BINARY ONLY: it must never
# reach asmtest-viewer's link line (D4 keeps the viewer permissive and
# engine-free), so STB_INC is added to the full app's flags alone.
STB_HOME ?= $(shell sh scripts/fetch-stb.sh)
STB_INC  := -I$(STB_HOME)
```

Add the test binary rule beside the other small ones (e.g. after
`$(BUILD)/desktop_test_nav`):

```make
$(BUILD)/desktop_test_png_write: $(BUILD)/desktop/test/ui/png_write.o \
    $(BUILD)/desktop/test/t/test_png_write.o
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@
```

Add `$(BUILD)/desktop_test_png_write` to the `DESKTOP_TESTS` list at ~line 1127.

The object rule for `desktop/src/ui/png_write.cpp` must carry `$(STB_INC)`.
Follow whatever pattern the neighbouring `ui/` objects use and append `$(STB_INC)`
to that one translation unit's flags.

- [ ] **Step 6: Run the test**

Run: `make build/desktop_test_png_write && ./build/desktop_test_png_write`
Expected: five `ok` lines and `test_png_write: all ok`

- [ ] **Step 7: Verify the viewer is untouched**

Run: `make desktop-render && ldd build/asmtest-viewer`
Expected: no new entries; the viewer must still show no engine libraries. Also
confirm `nm build/asmtest-viewer | grep -c stbi_write_png` prints `0`.

- [ ] **Step 8: Commit and push**

Paths: `scripts/fetch-stb.sh scripts/third-party-digests.txt licenses/stb-LICENSE.txt desktop/src/ui/png_write.h desktop/src/ui/png_write.cpp desktop/test/test_png_write.cpp mk/desktop.mk`
Subject: `desktop: pinned stb_image_write and the --shot PNG writer`

---

### Task 4: The shot manifest model

**Files:**
- Create: `desktop/src/ui/shot.h`, `desktop/src/ui/shot.cpp`
- Create: `desktop/test/test_shot_manifest.cpp`
- Create: `desktop/shots.json`
- Modify: `mk/desktop.mk` (test binary rule; `DESKTOP_TESTS` list)

**Interfaces:**
- Consumes: `scene3d::SceneKind` and `all_scene_kinds()` from `scene3d/scene_kind.h`; `scene3d::SceneLayers` and `scene_layers_all()` from `scene3d/scene.h` and `scene3d/layers.h`; `ViewId` from `ui/view_presence.h`.
- Produces, used by Task 5:
  ```cpp
  namespace asmdesk {
  struct ShotSpec {
      std::string name;                 // output basename, no extension
      std::vector<std::string> open;    // [0] = A side; [1] = B side (Divergence)
      ViewId view = ViewId::Scene3D;
      scene3d::SceneKind scene = scene3d::SceneKind::Plane;
      std::vector<std::string> layers;  // LayerDesc::id keys; empty = defaults
      int width = 1600, height = 1000, warmup = 30;
  };
  bool shot_manifest_parse(const std::string &json, std::vector<ShotSpec> &out,
                           std::string &err);
  bool shot_view_from_name(const std::string &name, ViewId &out);
  bool shot_scene_from_name(const std::string &name, scene3d::SceneKind &out);
  bool shot_apply_layers(const std::vector<std::string> &ids,
                         scene3d::SceneLayers &out, std::string &err);
  }
  ```

- [ ] **Step 1: Write the failing test**

Create `desktop/test/test_shot_manifest.cpp`:

```cpp
// test_shot_manifest.cpp — the screenshot manifest, and the rule that keeps the
// documentation honest: EVERY SceneKind must be covered by at least one shot.
//
// That exhaustiveness walk is the point. A new scene kind that nobody
// screenshotted fails this test rather than silently shipping a documented set
// that no longer documents everything — the same anti-drift discipline
// scene_axes() enforces for axis labels with its default-less switch.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "scene3d/scene_kind.h"
#include "ui/shot.h"

static int g_fail = 0;
static void check(const char *what, bool ok, const std::string &why) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        std::printf("     %s\n", why.c_str());
        g_fail = 1;
    }
}

static std::string slurp(const char *p) {
    std::ifstream f(p);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int main() {
    using namespace asmdesk;

    // --- parsing ------------------------------------------------------------
    {
        std::vector<ShotSpec> v;
        std::string err;
        bool ok = shot_manifest_parse(
            R"([{"name":"a","open":["x.asmtrace"],"view":"scene3d",
                 "scene":"LanePrism","layers":["terrain"],
                 "size":[800,600],"warmup":7}])",
            v, err);
        check("parse: a well-formed entry is accepted", ok, err);
        check("parse: name", ok && v.size() == 1 && v[0].name == "a", "name lost");
        check("parse: size and warmup",
              ok && v.size() == 1 && v[0].width == 800 && v[0].height == 600 &&
                  v[0].warmup == 7,
              "size/warmup not carried");
        check("parse: scene kind",
              ok && v.size() == 1 && v[0].scene == scene3d::SceneKind::LanePrism,
              "scene name did not map to its SceneKind");
    }
    {
        std::vector<ShotSpec> v;
        std::string err;
        bool ok = shot_manifest_parse(R"([{"name":"a","scene":"NoSuchScene"}])", v,
                                      err);
        check("parse: an unknown scene name is REFUSED, not defaulted",
              !ok && err.find("NoSuchScene") != std::string::npos,
              "an unknown scene silently became Plane — a shot would then "
              "document the wrong substrate");
    }
    {
        std::vector<ShotSpec> v;
        std::string err;
        check("parse: malformed JSON fails with a message",
              !shot_manifest_parse("{ not json", v, err) && !err.empty(),
              "a corrupt manifest must not parse to an empty shot list");
    }

    // --- layers -------------------------------------------------------------
    {
        scene3d::SceneLayers L;
        L.terrain = false;
        std::string err;
        check("layers: a known id is applied",
              shot_apply_layers({"terrain"}, L, err) && L.terrain, err);
        check("layers: an unknown id is REFUSED",
              !shot_apply_layers({"no-such-layer"}, L, err) &&
                  err.find("no-such-layer") != std::string::npos,
              "an unknown layer id was ignored instead of reported");
    }

    // --- the committed manifest --------------------------------------------
    {
        const std::string text = slurp("desktop/shots.json");
        check("manifest: desktop/shots.json is readable", !text.empty(),
              "run from the repo root");

        std::vector<ShotSpec> shots;
        std::string err;
        bool ok = shot_manifest_parse(text, shots, err);
        check("manifest: desktop/shots.json parses", ok, err);

        // EVERY SceneKind must appear. This is the anti-drift gate.
        for (scene3d::SceneKind k : scene3d::all_scene_kinds()) {
            bool covered = false;
            for (const ShotSpec &s : shots)
                if (s.view == ViewId::Scene3D && s.scene == k)
                    covered = true;
            check((std::string("manifest: SceneKind '") +
                   scene3d::scene_kind_name(k) + "' has a shot")
                      .c_str(),
                  covered,
                  std::string("no shot covers '") + scene3d::scene_kind_name(k) +
                      "' — add one to desktop/shots.json, or the documented set "
                      "no longer documents every scene");
        }

        // Names must be unique, or one shot silently overwrites another's PNG.
        bool uniq = true;
        for (size_t i = 0; i < shots.size(); i++)
            for (size_t j = i + 1; j < shots.size(); j++)
                if (shots[i].name == shots[j].name)
                    uniq = false;
        check("manifest: shot names are unique", uniq,
              "two shots share a name and would overwrite each other");
    }

    std::printf("%s\n",
                g_fail ? "test_shot_manifest: FAILURES" : "test_shot_manifest: all ok");
    return g_fail;
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `make build/desktop_test_shot_manifest`
Expected: build failure — `ui/shot.h: No such file or directory`

- [ ] **Step 3: Write the model**

Create `desktop/src/ui/shot.h`:

```cpp
// shot.h — the screenshot manifest: WHICH recording, WHICH view, WHICH scene
// substrate and layers each documented image is rendered from.
//
// A manifest rather than CLI flags so the whole set costs one process and one GL
// context, and so adding or reordering an image is a data edit. The model here
// is PURE — no GL, no ImGui, no EGL — in the same split every view in this tree
// uses, which is what lets test_shot_manifest assert the exhaustiveness rule
// headlessly.
#ifndef ASMDESK_UI_SHOT_H
#define ASMDESK_UI_SHOT_H

#include <string>
#include <vector>

#include "scene3d/scene.h"      // SceneLayers
#include "scene3d/scene_kind.h" // SceneKind
#include "ui/view_presence.h"   // ViewId

namespace asmdesk {

struct ShotSpec {
    std::string name; // output basename, no extension
    // The recordings to open. [0] is the A side. [1], when present, is attached
    // as the B side — the role the `d` key fills interactively, and the only way
    // the Divergence scene has anything to diverge FROM.
    std::vector<std::string> open;
    ViewId view = ViewId::Scene3D;
    scene3d::SceneKind scene = scene3d::SceneKind::Plane;
    std::vector<std::string> layers; // LayerDesc::id keys; empty => defaults
    int width = 1600;
    int height = 1000;
    // Frames rendered before the capture. ImGui's docking and layout settle over
    // several frames, and the terrain weave lands over a few more; capturing
    // frame 0 photographs a half-built UI.
    int warmup = 30;
};

// Parse a manifest (a JSON array of entries). Returns false with `err` set on
// malformed JSON, an unknown view/scene/layer name, or a missing `name`.
// An unknown name is REFUSED rather than defaulted: a shot that silently
// documented a different substrate is worse than no shot.
bool shot_manifest_parse(const std::string &json, std::vector<ShotSpec> &out,
                         std::string &err);

// Name -> enum. False (leaving `out` untouched) if the name is not known.
bool shot_view_from_name(const std::string &name, ViewId &out);
bool shot_scene_from_name(const std::string &name, scene3d::SceneKind &out);

// Turn ON each named layer, by the stable LayerDesc::id key. Returns false with
// `err` naming the first unknown id.
bool shot_apply_layers(const std::vector<std::string> &ids,
                       scene3d::SceneLayers &out, std::string &err);

} // namespace asmdesk
#endif // ASMDESK_UI_SHOT_H
```

Create `desktop/src/ui/shot.cpp`:

```cpp
#include "ui/shot.h"

#include <nlohmann/json.hpp>

#include "scene3d/layers.h" // scene_layers_all()

namespace asmdesk {

bool shot_view_from_name(const std::string &n, ViewId &out) {
    if (n == "summary")   { out = ViewId::Summary;  return true; }
    if (n == "canvas")    { out = ViewId::Canvas;   return true; }
    if (n == "timeline")  { out = ViewId::Timeline; return true; }
    if (n == "slice")     { out = ViewId::Slice;    return true; }
    if (n == "diff")      { out = ViewId::Diff;     return true; }
    if (n == "observer")  { out = ViewId::Observer; return true; }
    if (n == "loom")      { out = ViewId::Loom;     return true; }
    if (n == "scrubber")  { out = ViewId::Scrubber; return true; }
    if (n == "abixray")   { out = ViewId::AbiXray;  return true; }
    if (n == "scene3d")   { out = ViewId::Scene3D;  return true; }
    return false;
}

bool shot_scene_from_name(const std::string &n, scene3d::SceneKind &out) {
    using K = scene3d::SceneKind;
    if (n == "Plane")        { out = K::Plane;        return true; }
    if (n == "Divergence")   { out = K::Divergence;   return true; }
    if (n == "Invocation")   { out = K::Invocation;   return true; }
    if (n == "ModuleRibbon") { out = K::ModuleRibbon; return true; }
    if (n == "LanePrism")    { out = K::LanePrism;    return true; }
    return false;
}

bool shot_apply_layers(const std::vector<std::string> &ids,
                       scene3d::SceneLayers &out, std::string &err) {
    err.clear();
    for (const std::string &id : ids) {
        bool found = false;
        for (const scene3d::LayerDesc &d : scene3d::scene_layers_all())
            if (id == d.id) {
                out.*(d.flag) = true;
                found = true;
                break;
            }
        if (!found) {
            err = "unknown layer id \"" + id + "\"";
            return false;
        }
    }
    return true;
}

bool shot_manifest_parse(const std::string &text, std::vector<ShotSpec> &out,
                         std::string &err) {
    out.clear();
    err.clear();

    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_array()) {
        err = "shot manifest: not a JSON array";
        return false;
    }

    for (const nlohmann::json &e : j) {
        if (!e.is_object()) {
            err = "shot manifest: an entry is not an object";
            return false;
        }
        ShotSpec s;
        s.name = e.value("name", std::string());
        if (s.name.empty()) {
            err = "shot manifest: an entry has no \"name\"";
            return false;
        }
        if (e.contains("open") && e["open"].is_array())
            for (const nlohmann::json &p : e["open"])
                if (p.is_string())
                    s.open.push_back(p.get<std::string>());

        if (e.contains("view")) {
            const std::string v = e.value("view", std::string());
            if (!shot_view_from_name(v, s.view)) {
                err = "shot manifest: unknown view \"" + v + "\" in \"" + s.name + "\"";
                return false;
            }
        }
        if (e.contains("scene")) {
            const std::string k = e.value("scene", std::string());
            if (!shot_scene_from_name(k, s.scene)) {
                err = "shot manifest: unknown scene \"" + k + "\" in \"" + s.name + "\"";
                return false;
            }
        }
        if (e.contains("layers") && e["layers"].is_array())
            for (const nlohmann::json &l : e["layers"])
                if (l.is_string())
                    s.layers.push_back(l.get<std::string>());

        if (e.contains("size") && e["size"].is_array() && e["size"].size() == 2) {
            s.width = e["size"][0].get<int>();
            s.height = e["size"][1].get<int>();
        }
        s.warmup = e.value("warmup", s.warmup);

        if (s.width <= 0 || s.height <= 0) {
            err = "shot manifest: non-positive size in \"" + s.name + "\"";
            return false;
        }
        // Validate layer ids now, so a typo fails the manifest test rather than
        // silently producing an image with a layer the author meant to enable.
        scene3d::SceneLayers probe;
        std::string lerr;
        if (!shot_apply_layers(s.layers, probe, lerr)) {
            err = "shot manifest: " + lerr + " in \"" + s.name + "\"";
            return false;
        }
        out.push_back(std::move(s));
    }
    return true;
}

} // namespace asmdesk
```

- [ ] **Step 4: Write the initial manifest**

Create `desktop/shots.json` with the full set. Every `SceneKind` must appear or
the test fails:

```json
[
  { "name": "01-entry-rail",     "open": ["build/shots/rec/df-a.asmtrace"],
    "view": "summary",  "size": [1600, 1000], "warmup": 30 },
  { "name": "02-timeline",       "open": ["build/shots/rec/df-a.asmtrace"],
    "view": "timeline", "size": [1600, 1000], "warmup": 30 },
  { "name": "03-scrubber",       "open": ["build/shots/rec/df-a.asmtrace"],
    "view": "scrubber", "size": [1600, 1000], "warmup": 30 },
  { "name": "04-canvas",         "open": ["build/shots/rec/df-a.asmtrace"],
    "view": "canvas",   "size": [1600, 1000], "warmup": 30 },
  { "name": "05-loom",           "open": ["build/shots/rec/df-a.asmtrace"],
    "view": "loom",     "size": [1600, 1000], "warmup": 30 },

  { "name": "10-plane-bare",     "open": ["build/shots/rec/df-a.asmtrace"],
    "view": "scene3d",  "scene": "Plane",
    "layers": ["terrain", "exact"], "size": [1600, 1000], "warmup": 30 },
  { "name": "11-plane-zoning",   "open": ["build/shots/rec/df-a.asmtrace"],
    "view": "scene3d",  "scene": "Plane",
    "layers": ["terrain", "exact", "zoning", "contours"],
    "size": [1600, 1000], "warmup": 30 },
  { "name": "12-plane-canopy",   "open": ["build/shots/rec/df-a.asmtrace"],
    "view": "scene3d",  "scene": "Plane",
    "layers": ["terrain", "exact", "canopy", "opcode"],
    "size": [1600, 1000], "warmup": 30 },
  { "name": "13-plane-access",   "open": ["build/shots/rec/df-a.asmtrace"],
    "view": "scene3d",  "scene": "Plane",
    "layers": ["terrain", "exact", "access", "relief"],
    "size": [1600, 1000], "warmup": 30 },
  { "name": "14-plane-weather",  "open": ["build/shots/rec/df-a.asmtrace"],
    "view": "scene3d",  "scene": "Plane",
    "layers": ["terrain", "exact", "weather", "ghost fog"],
    "size": [1600, 1000], "warmup": 30 },

  { "name": "20-invocation",     "open": ["build/shots/rec/trace-blend.asmtrace"],
    "view": "scene3d",  "scene": "Invocation", "size": [1600, 1000], "warmup": 30 },
  { "name": "21-module-ribbon",  "open": ["build/shots/rec/tree.asmtrace"],
    "view": "scene3d",  "scene": "ModuleRibbon", "size": [1600, 1000], "warmup": 30 },
  { "name": "22-lane-prism",     "open": ["build/shots/rec/df-a.asmtrace"],
    "view": "scene3d",  "scene": "LanePrism", "size": [1600, 1000], "warmup": 30 },
  { "name": "23-divergence",
    "open": ["build/shots/rec/df-a.asmtrace", "build/shots/rec/df-b.asmtrace"],
    "view": "scene3d",  "scene": "Divergence", "size": [1600, 1000], "warmup": 30 }
]
```

**If a layer id above is rejected by the test**, the id is wrong — read the real
keys out of `scene_layers_all()` in `desktop/src/scene3d/layers.cpp` and correct
the manifest. Do not delete the layer from the shot to make the test pass.

- [ ] **Step 5: Wire the build**

In `mk/desktop.mk`, add the test rule:

```make
$(BUILD)/desktop_test_shot_manifest: $(BUILD)/desktop/test/ui/shot.o \
    $(BUILD)/desktop/test/s3/layers.o \
    $(BUILD)/desktop/test/t/test_shot_manifest.o
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@
```

Add `$(BUILD)/desktop_test_shot_manifest` to `DESKTOP_TESTS`.

- [ ] **Step 6: Run the test**

Run: `make build/desktop_test_shot_manifest && ./build/desktop_test_shot_manifest`
Expected: every line `ok`, including one per SceneKind, and `test_shot_manifest: all ok`

- [ ] **Step 7: Prove the exhaustiveness gate actually bites**

Temporarily delete the `22-lane-prism` entry from `desktop/shots.json` and re-run
the test.
Expected: `FAIL manifest: SceneKind 'SIMD lane prism' has a shot`
Then restore the entry and confirm the test passes again. A gate that cannot fail
is not a gate.

- [ ] **Step 8: Commit and push**

Paths: `desktop/src/ui/shot.h desktop/src/ui/shot.cpp desktop/test/test_shot_manifest.cpp desktop/shots.json mk/desktop.mk`
Subject: `desktop: the screenshot manifest, with a scene-kind exhaustiveness gate`

---

### Task 5: Offscreen capture and the `--shot` entry point

**Files:**
- Create: `desktop/src/ui/shot_render.h`, `desktop/src/ui/shot_render.cpp`
- Create: `desktop/src/ui/shot_render_stub.cpp` (the no-EGL-headers build)
- Modify: `desktop/src/main.cpp` (signature `int main(int, char **)`; dispatch `--shot`)
- Modify: `mk/desktop.mk` (`DESKTOP_UI_APP` app-only object list ~line 650; `$(EGL_LIBS)` on the app link line ~line 961)

**Interfaces:**
- Consumes: `ShotSpec` and the parse/apply helpers from Task 4; `png_write_rgba_flipped` from Task 3; `asmdesk::draw_shell(ShellState&)`, `asmdesk::make_gl_scene_host()`, `Workspace::open`, `ShellState::want_view_id`, `ShellState::scenes[i].hud.req_kind`, `ShellState::scenes[i].hud.layers`.
- Produces:
  ```cpp
  namespace asmdesk {
  // Render every shot in `manifest_path` into `out_dir`. Returns 0 on success,
  // non-zero on the first failure (message already printed to stderr).
  int shot_run(const std::string &manifest_path, const std::string &out_dir);
  }
  ```

- [ ] **Step 1: Write the capture host**

Create `desktop/src/ui/shot_render.h`:

```cpp
// shot_render.h — the --shot offscreen capture host.
//
// Renders the REAL shell (ui/shell.cpp's draw_shell) through the REAL OpenGL3
// ImGui backend into a surfaceless-EGL framebuffer, then reads it back as PNG.
//
// Surfaceless EGL rather than a hidden GLFW window: it needs no X display at
// all, so the same command works on a desktop session, over ssh, and inside the
// docker-desktop lane without Xvfb. desktop/test/test_scene_fbo.cpp already
// proves this exact path (surfaceless context, FBO, glReadPixels) in this tree.
//
// ImGui's PLATFORM backend is replaced by setting DisplaySize/DeltaTime by hand
// — the standard ImGui pattern, and the same shape the null-backend tests use.
// The RENDERER backend stays the real imgui_impl_opengl3, so these are the
// pixels the app ships.
//
// FULL BINARY ONLY: never compiled into asmtest-viewer (D4).
#ifndef ASMDESK_UI_SHOT_RENDER_H
#define ASMDESK_UI_SHOT_RENDER_H

#include <string>

namespace asmdesk {
int shot_run(const std::string &manifest_path, const std::string &out_dir);
}
#endif // ASMDESK_UI_SHOT_RENDER_H
```

Create `desktop/src/ui/shot_render.cpp`. Model the EGL and FBO setup on
`desktop/test/test_scene_fbo.cpp` lines ~480-570 (`eglGetPlatformDisplayEXT`
with `EGL_PLATFORM_SURFACELESS_MESA`, `eglBindAPI(EGL_OPENGL_API)`, a 3.3
context, `glGenFramebuffers` + colour texture + depth renderbuffer):

```cpp
#include "ui/shot_render.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"
#include "imsearch.h"

#include "ui/gl_scene_host.h"
#include "ui/png_write.h"
#include "ui/shell.h"
#include "ui/shot.h"
#include "ui/theme.h"

namespace asmdesk {
namespace {

void say(const std::string &m) { std::fprintf(stderr, "shot: %s\n", m.c_str()); }

struct Egl {
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLContext ctx = EGL_NO_CONTEXT;
    bool ok = false;

    bool init(std::string &err) {
        auto getPD = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress(
            "eglGetPlatformDisplayEXT");
        if (getPD == nullptr) {
            err = "eglGetPlatformDisplayEXT unavailable";
            return false;
        }
        dpy = getPD(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
        if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, nullptr, nullptr)) {
            err = "no surfaceless EGL display";
            return false;
        }
        eglBindAPI(EGL_OPENGL_API);
        EGLint cfga[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                         EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
        EGLConfig cfg;
        EGLint n = 0;
        if (!eglChooseConfig(dpy, cfga, &cfg, 1, &n) || n < 1) {
            err = "no usable EGL config";
            return false;
        }
        EGLint ctxa[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION,
                         3, EGL_NONE};
        ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxa);
        if (ctx == EGL_NO_CONTEXT) {
            err = "could not create a GL 3.3 context";
            return false;
        }
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
        ok = true;
        return true;
    }
    ~Egl() {
        if (ok) {
            eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroyContext(dpy, ctx);
            eglTerminate(dpy);
        }
    }
};

struct Fbo {
    GLuint fbo = 0, tex = 0, rb = 0;
    int w = 0, h = 0;
    bool make(int W, int H) {
        destroy();
        w = W; h = H;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glGenRenderbuffers(1, &rb);
        glBindRenderbuffer(GL_RENDERBUFFER, rb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, W, H);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, tex, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                  GL_RENDERBUFFER, rb);
        return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }
    void destroy() {
        if (fbo) glDeleteFramebuffers(1, &fbo);
        if (tex) glDeleteTextures(1, &tex);
        if (rb)  glDeleteRenderbuffers(1, &rb);
        fbo = tex = rb = 0;
    }
    ~Fbo() { destroy(); }
};

std::string slurp(const std::string &p) {
    std::ifstream f(p);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

int shot_run(const std::string &manifest_path, const std::string &out_dir) {
    const std::string text = slurp(manifest_path);
    if (text.empty()) {
        say("cannot read manifest " + manifest_path);
        return 1;
    }
    std::vector<ShotSpec> shots;
    std::string err;
    if (!shot_manifest_parse(text, shots, err)) {
        say(err);
        return 1;
    }

    Egl egl;
    if (!egl.init(err)) {
        say(err + " — --shot needs a working EGL/GL 3.3 stack");
        return 1;
    }
    say(std::string("GL renderer: ") +
        reinterpret_cast<const char *>(glGetString(GL_RENDERER)));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImSearch::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    // Deterministic chrome: no .ini so a developer's dock layout cannot leak
    // into a committed image, and no log file.
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    dt_set_light_theme(false);
    ImGui::StyleColorsDark();
    ImGui_ImplOpenGL3_Init("#version 130");

    int rc = 0;
    for (const ShotSpec &s : shots) {
        Fbo fb;
        if (!fb.make(s.width, s.height)) {
            say("incomplete framebuffer for " + s.name);
            rc = 1;
            break;
        }

        // A FRESH ShellState per shot: no persisted workspace, no settings
        // store, nothing carried from the previous image.
        ShellState st;
        std::unique_ptr<SceneHost> host = make_gl_scene_host();
        host->init();
        st.scene_host = host.get();

        bool opened = true;
        for (const std::string &p : s.open) {
            std::string oerr;
            if (st.ws.open(p, oerr) < 0) {
                say("cannot open " + p + ": " + oerr);
                opened = false;
                break;
            }
        }
        if (!opened) { rc = 1; break; }
        st.active_tab = 0;

        for (int frame = 0; frame <= s.warmup; frame++) {
            io.DisplaySize = ImVec2((float)s.width, (float)s.height);
            io.DeltaTime = 1.0f / 60.0f;

            ImGui_ImplOpenGL3_NewFrame();
            ImGui::NewFrame();

            // Re-assert the intent every frame: the shell consumes and resets
            // want_view_id at the end of each frame (shell.cpp ~3984).
            st.want_view_id = s.view;
            if (st.active_tab >= 0 &&
                static_cast<size_t>(st.active_tab) < st.scenes.size()) {
                SceneView &sv = st.scenes[static_cast<size_t>(st.active_tab)];
                // BOTH fields, every frame. req_kind alone does NOTHING: the
                // shell only acts on it when req_kind_change is set, then
                // clears the flag (shell.cpp:1272-1279). Setting the value
                // without the flag leaves every 3D shot on the default Plane —
                // a silent wrong-substrate bug, not a visible failure.
                sv.hud.req_kind = s.scene;
                sv.hud.req_kind_change = true;
                if (!s.layers.empty()) {
                    sv.hud.layers = scene3d::SceneLayers{};
                    std::string lerr;
                    shot_apply_layers(s.layers, sv.hud.layers, lerr);
                }
            }

            draw_shell(st);

            ImGui::Render();
            glBindFramebuffer(GL_FRAMEBUFFER, fb.fbo);
            glViewport(0, 0, s.width, s.height);
            glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        }

        std::vector<unsigned char> px(
            static_cast<size_t>(s.width) * s.height * 4);
        glBindFramebuffer(GL_FRAMEBUFFER, fb.fbo);
        glReadPixels(0, 0, s.width, s.height, GL_RGBA, GL_UNSIGNED_BYTE,
                     px.data());
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        const std::string path = out_dir + "/" + s.name + ".png";
        std::string werr;
        if (!png_write_rgba_flipped(path, s.width, s.height, px.data(), werr)) {
            say(werr);
            rc = 1;
            break;
        }
        say("wrote " + path);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImSearch::DestroyContext();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    return rc;
}

} // namespace asmdesk
```

- [ ] **Step 2: Wire `--shot` into main**

In `desktop/src/main.cpp`, change `int main() {` (line 103) to
`int main(int argc, char **argv) {` and insert immediately after it:

```cpp
#ifndef ASMTEST_DESKTOP_RENDER_ONLY
    // --shot <manifest.json> --out <dir>: render the documented screenshots
    // offscreen and exit, without ever creating a window. Full binary only —
    // the permissive viewer links no EGL and no PNG encoder (D4).
    {
        std::string shot_manifest, shot_out = "build/shots";
        for (int i = 1; i < argc; i++) {
            if (std::strcmp(argv[i], "--shot") == 0 && i + 1 < argc)
                shot_manifest = argv[++i];
            else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc)
                shot_out = argv[++i];
        }
        if (!shot_manifest.empty())
            return asmdesk::shot_run(shot_manifest, shot_out);
    }
#endif
```

Add `#include <cstring>` and, guarded by the same `#ifndef`, `#include "ui/shot_render.h"`.

- [ ] **Step 3: Wire the build**

`mk/desktop.mk` already has the exact precedent for an app-only object:
`DESKTOP_VIEW_APP := regsynth` at ~line 646, which is app-only *because* it links
an engine and would otherwise contaminate the viewer. Follow it literally.

Immediately after the `DESKTOP_RENDER_OBJ := $(call desktop_app_objs,render)`
line (~650), add:

```make
# --shot (gui screenshots): the manifest model, the EGL/FBO capture host and the
# PNG encoder. APP-ONLY for the same reason regsynth is: asmtest-viewer is the
# permissively-distributable render-only binary and must gain no EGL linkage and
# no stb_image_write (D4). Adding these to desktop_app_objs would put them in
# BOTH trees — the whole point of listing them here instead.
DESKTOP_UI_APP  := shot shot_render png_write
DESKTOP_APP_OBJ += $(DESKTOP_UI_APP:%=$(BUILD)/desktop/app/ui/%.o)

# The two TUs that need third-party headers, and only those two.
$(BUILD)/desktop/app/ui/png_write.o: DESKTOP_CXXFLAGS += $(STB_INC)
```

Then add `$(EGL_LIBS)` to the `$(BUILD)/asmtest-desktop` link line (~line 961),
after `$(GL_LIBS)`:

```make
	  $(GLFW_LIBS) $(GL_LIBS) $(EGL_LIBS) $(FREETYPE_LIBS) $(X11_LIBS) -ldl -lpthread -o $@
```

Leave `$(BUILD)/asmtest-viewer`'s recipe untouched.

**Gate on the EGL headers.** `DESKTOP_GL_MISSING` (~line 782) is already the
variable that reports absent GL/EGL headers, and the FBO smoke is gated on it at
~line 2224. Wrap the three additions above in the same
`ifeq ($(strip $(DESKTOP_GL_MISSING)),)` guard, and in the `else` branch compile
a `shot_render_stub.cpp` whose `shot_run` prints:

```
shot: this build has no EGL headers, so --shot was not compiled in
      (make docker-desktop installs software Mesa + EGL)
```

and returns 1. A bare host must still build a working `asmtest-desktop` — it just
cannot take screenshots, and it says so rather than failing to link.

- [ ] **Step 4: Build and smoke it**

Run:
```bash
make desktop
mkdir -p build/shots
./build/asmtest-desktop --shot desktop/shots.json --out build/shots
```
Expected: a `shot: GL renderer: …` line, then one `shot: wrote …` per manifest
entry, exit status 0.

If it exits with `no surfaceless EGL display`, the host lacks EGL — run it in the
`docker-desktop` lane instead, which installs software Mesa and EGL.

- [ ] **Step 5: Verify the viewer is still clean**

Run: `make desktop-render && ldd build/asmtest-viewer | grep -ci egl`
Expected: `0`. Also `./build/asmtest-viewer --shot x --out y` must ignore the
flags and open the GUI normally, because the block is compiled out there.

- [ ] **Step 6: Commit and push**

Paths: `desktop/src/ui/shot_render.h desktop/src/ui/shot_render.cpp desktop/src/main.cpp mk/desktop.mk`
Subject: `desktop: --shot renders the documented screenshots offscreen via EGL`

---

### Task 6: The `gui-shots` target and the non-blank gate

**Files:**
- Create: `scripts/verify-shots.py`
- Modify: `mk/desktop.mk` (`gui-shots` target)

**Interfaces:**
- Consumes: `build/shots/*.png` from Task 5, `desktop/shots.json` from Task 4.
- Produces: verified PNGs ready to be copied into `docs/_static/gui/`.

- [ ] **Step 1: Write the failing test**

Create `scripts/verify-shots.py`:

```python
#!/usr/bin/env python3
"""verify-shots.py — every manifest entry produced a real image.

Two failures this exists to catch, neither visible at the file level:

1. The GL context dies, every capture reads back a uniform buffer, and fourteen
   black rectangles get committed as documentation.
2. The scene kind never actually switches, so every 3D shot is the default
   address plane under a different filename. These images are all non-blank and
   correctly sized; only comparing them to each other reveals it. (This is a
   real bug shape: SceneView::hud.req_kind is inert unless req_kind_change is
   also set.)"""
import hashlib
import json
import struct
import sys
import zlib

MANIFEST = sys.argv[1] if len(sys.argv) > 1 else "desktop/shots.json"
SHOTS = sys.argv[2] if len(sys.argv) > 2 else "build/shots"

def png_info(path):
    """Return (width, height, distinct_colour_count) using only stdlib."""
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    w, h = struct.unpack(">II", data[16:24])
    bitd, colr = data[24], data[25]
    if bitd != 8 or colr != 6:
        raise ValueError(f"expected 8-bit RGBA, got bitdepth={bitd} colour={colr}")

    idat, pos = b"", 8
    while pos < len(data):
        (ln,) = struct.unpack(">I", data[pos:pos + 4])
        typ = data[pos + 4:pos + 8]
        if typ == b"IDAT":
            idat += data[pos + 8:pos + 8 + ln]
        pos += 12 + ln
    raw = zlib.decompress(idat)

    # Un-filter enough to sample colours. Stride is 4 bytes/px + 1 filter byte.
    stride = w * 4
    out, prev = bytearray(), bytearray(stride)
    p = 0
    for _ in range(h):
        f = raw[p]; p += 1
        line = bytearray(raw[p:p + stride]); p += stride
        for i in range(stride):
            a = line[i - 4] if i >= 4 else 0
            b = prev[i]
            c = prev[i - 4] if i >= 4 else 0
            if f == 1: line[i] = (line[i] + a) & 0xFF
            elif f == 2: line[i] = (line[i] + b) & 0xFF
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 0xFF
            elif f == 4:
                pp = a + b - c
                pa, pb, pc = abs(pp - a), abs(pp - b), abs(pp - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        out += line
        prev = line

    colours = set()
    for y in range(0, h, 7):          # sample; full scan is needlessly slow
        for x in range(0, w, 7):
            o = y * stride + x * 4
            colours.add(bytes(out[o:o + 3]))
    return w, h, len(colours)

with open(MANIFEST, encoding="utf-8") as fh:
    shots = json.load(fh)

failures = []
digests = {}
for s in shots:
    path = f"{SHOTS}/{s['name']}.png"
    try:
        w, h, ncol = png_info(path)
    except (OSError, ValueError) as exc:
        failures.append(f"{s['name']}: {exc}")
        continue
    want_w, want_h = s.get("size", [1600, 1000])
    if (w, h) != (want_w, want_h):
        failures.append(f"{s['name']}: {w}x{h}, manifest says {want_w}x{want_h}")
    if ncol < 8:
        failures.append(
            f"{s['name']}: only {ncol} distinct sampled colours — "
            "this image is blank or near-blank, not a screenshot")
    with open(path, "rb") as fh:
        digests[s["name"]] = hashlib.sha256(fh.read()).hexdigest()

# Every 3D shot must differ from every other 3D shot. Two identical images mean
# the scene kind or the layer set never actually changed between them, which no
# amount of size- or blankness-checking would reveal.
scene_shots = [s["name"] for s in shots
               if s.get("view") == "scene3d" and s["name"] in digests]
for i, a in enumerate(scene_shots):
    for b in scene_shots[i + 1:]:
        if digests[a] == digests[b]:
            failures.append(
                f"{a} and {b} are byte-identical — the scene kind or layer set "
                "did not change between them (check req_kind_change)")

if failures:
    print("FAIL:")
    for f in failures:
        print("  " + f)
    sys.exit(1)
print(f"PASS: {len(shots)} shots, correctly sized, non-blank, "
      f"and {len(scene_shots)} 3D scenes all distinct")
```

- [ ] **Step 2: Run it to verify it fails**

Run: `rm -f build/shots/*.png && python3 scripts/verify-shots.py`
Expected: `FAIL:` listing every shot as missing.

- [ ] **Step 3: Add the target**

In `mk/desktop.mk`:

```make
# Render the documented screenshots and PROVE they are real images. The blank
# check is the point: a dead GL context writes correctly-sized black rectangles
# that look fine until someone opens the docs.
gui-shots: $(BUILD)/asmtest-desktop
	@mkdir -p $(BUILD)/shots
	./$(BUILD)/asmtest-desktop --shot desktop/shots.json --out $(BUILD)/shots
	python3 scripts/verify-shots.py desktop/shots.json $(BUILD)/shots
```

- [ ] **Step 4: Run it**

Run: `make gui-shots`
Expected: `PASS: 14 shots, correctly sized, non-blank, and 9 3D scenes all distinct`

A shot that fails the blank check has a real problem — an unavailable scene, a
recording that did not load, or a camera pointed at nothing. Investigate the
image; do not lower the colour threshold.

If the **distinctness** check fails, the scene kind or layer set is not being
applied. Check that Task 5's loop sets `sv.hud.req_kind_change = true` alongside
`sv.hud.req_kind`, and open the two named images side by side to confirm.

- [ ] **Step 4b: Prove the distinctness gate bites**

Temporarily comment out the `sv.hud.req_kind_change = true;` line in
`desktop/src/ui/shot_render.cpp`, rebuild, and run `make gui-shots`.
Expected: FAIL, naming pairs of byte-identical 3D shots.
Restore the line, rebuild, and confirm the check passes again.

- [ ] **Step 5: Commit and push**

Paths: `scripts/verify-shots.py mk/desktop.mk`
Subject: `build: gui-shots renders and verifies the documentation screenshots`

---

### Task 7: The guide page

**Files:**
- Create: `docs/guides/desktop-gui-scenes.md`
- Create: `docs/_static/gui/*.png` (copied from `build/shots/`)
- Modify: `docs/index.md` (Guides toctree)

**Interfaces:**
- Consumes: the verified PNGs from Task 6; `scene_axes()` in `desktop/src/scene3d/scene_kind.h` for each kind's axis wording.

- [ ] **Step 1: Copy the verified images**

```bash
mkdir -p docs/_static/gui
cp build/shots/*.png docs/_static/gui/
python3 scripts/verify-shots.py desktop/shots.json docs/_static/gui
```
Expected: `PASS: 14 shots, all correctly sized and non-blank`

- [ ] **Step 2: Read the real axis wording**

Run: `sed -n '/scene_axes/,/^}/p' desktop/src/scene3d/scene_kind.h`

Copy each kind's axis labels and its `y_not` string **verbatim** into the page.
Do not paraphrase them: `y_not` exists specifically to state what an axis is
*not*, and a paraphrase is exactly the fabrication the field prevents.

- [ ] **Step 3: Write the page**

Create `docs/guides/desktop-gui-scenes.md` with this structure. Fill each scene
section with the image, the verbatim axis wording from Step 2, and the recording
that fills it:

````markdown
# The desktop GUI and its 3D scenes

[Opening: what the 3D pane is — one screen per question, over a recording.]

## Two time axes, deliberately not fused

[The exec-step axis and the terrain trace-time axis measure different things.
State this up front: it is the single most common misreading of these images.]

## The tour

![The entry rail](../_static/gui/01-entry-rail.png)
[...Summary, Timeline, Scrubber, Canvas, Loom, one short paragraph each...]

## The address plane

![The bare plane](../_static/gui/10-plane-bare.png)

[What X, Z and height mean. Then one subsection per layer group with its image:
zoning/contours, canopy/opcode, access/relief, weather/ghost-fog.]

## Scenes whose axes are not addresses

[Why these are separate substrates and do not compose: two meanings on one
screen position is what the layer registry exists to avoid.]

### Invocation
![Invocation](../_static/gui/20-invocation.png)
[Verbatim axes + y_not. Filled by `trace-blend.asmtrace`: an `--trace` capture of
`blend_tile`, where each `coverage` event closes one invocation slab.]

### Module ribbon
![Module ribbon](../_static/gui/21-module-ribbon.png)
[Verbatim axes + y_not. Filled by `tree.asmtrace`: a `--tree` capture. No golden
recording carries `call` events, so this scene requires a live capture.]

### Lane prism
![Lane prism](../_static/gui/22-lane-prism.png)
[Verbatim axes + y_not. Filled by `df-a.asmtrace`. Note that lane width comes
from the MNEMONIC — the recording does not carry SIMD element width — so a
mnemonic the classifier cannot name renders at a default width and says so.]

### Divergence
![Divergence](../_static/gui/23-divergence.png)
[Verbatim axes + y_not. Needs TWO recordings with a matching `code_sha`; the two
sides differ only in the `--seed` passed to the sample process.]

## Reproducing these

### 1. Set up the host

Attaching to a process you did not launch needs permissions a hardened desktop
Linux does not grant by default. See
[Host setup for tracing](../getting-started/host-setup.md).

On the reference host these images were made on, the two gates measured:

- `kernel.yama.ptrace_scope` was `1` (the Ubuntu default). The sample process
  calls `PR_SET_PTRACER_ANY`, which is what lets the attach succeed without
  changing the host at all.
- `kernel.perf_event_paranoid` was `4`, so IBS was unavailable and `--sample` /
  `--auto --sampler=ibs` could not be used. Read yours with
  `sysctl kernel.perf_event_paranoid`.

### 2. Build and capture

```
make cli desktop
make gui-shot-recordings
make gui-shots
```

`gui-shot-recordings` starts the sample process and attaches four times. **Four
recordings, not one**, and this is structural: `call` events and `df_*` events
come from different engines and cannot share a recording.

| Recording | Command | Fills |
|---|---|---|
| `tree.asmtrace` | `--tree <pid> 400` | Module ribbon |
| `trace-blend.asmtrace` | `--trace <pid> blend_tile 12` | Invocation |
| `df-a.asmtrace` | `--dataflow <pid> blend_tile --steps --mem --fpregs --statediff` | Plane, lane prism, divergence A, Scrubber |
| `df-b.asmtrace` | the same, against a `--seed 2` process | Divergence B |

`--fpregs` is there for the Scrubber's wide register deck. The lane prism does
not need it: the dataflow producer reads XMM operands directly.

### 3. The sample process

[Describe cli/scenes_victim.c: what each part is for, and why --seed changes data
and never code.]
````

- [ ] **Step 4: Add to the toctree**

In `docs/index.md`, add `guides/desktop-gui-scenes` to the Guides toctree,
after `guides/emulator`.

- [ ] **Step 5: Build the docs**

Run: `rm -rf docs/_build/doctrees && make docker-docs`
Expected: the build reaches `build succeeded` with **no new warnings**.

There is one **pre-existing** warning in this tree — a `CHANGELOG.md`
cross-reference to `docs/getting-started/installation.md#install-the-desktop-gui-app`.
It reproduces on a pristine checkout and is not yours. Every *other* warning is.

- [ ] **Step 6: Confirm the images render**

Run: `ls -la docs/_build/html/_static/gui/*.png | head`
Expected: all 14 files present and non-trivial in size.

- [ ] **Step 7: Commit and push**

Paths: `docs/guides/desktop-gui-scenes.md docs/_static/gui docs/index.md`
Subject: `docs: the desktop GUI 3D scenes guide, with regenerable screenshots`

---

## Self-Review

**Spec coverage:**

| Spec component | Task |
|---|---|
| `cli/scenes_victim.c` shaped to every gate | 1 |
| `--seed` changes data only (Divergence) | 1 (code), 2 (`df-b` capture), 4 (manifest entry) |
| Four recordings, structurally necessary | 2 |
| Pinned `stb_image_write` + digest row | 3 |
| PNG writer, full binary only | 3 |
| Manifest model, scene/layer/view name binding | 4 |
| Scene-kind exhaustiveness gate | 4 (steps 1, 7) |
| Surfaceless EGL + FBO + real `draw_shell` | 5 |
| Determinism: no `.ini`, fresh `ShellState`, fixed warmup | 5 |
| `--shot` never in the permissive viewer | 3 (step 7), 5 (steps 2, 5) |
| Non-blank / expected-dimension per PNG | 6 |
| Guide page with verbatim axis wording | 7 |
| Host-setup framing (measured, with the general rule) | 7 (step 3) |

**Known deviation from the spec:** the spec listed a provenance sidecar recording
the GL renderer string. Task 5 prints it to stderr instead, which `make gui-shots`
captures in its log. If a committed sidecar file is wanted, add it as a follow-up
— it is a one-line write in `shot_run`.

**Two bugs this review caught before they were written:**

1. `sv.hud.req_kind` is **inert on its own** — `shell.cpp:1272-1279` only acts on
   it when `req_kind_change` is also set, then clears the flag. The first draft
   set only the value, which would have rendered every 3D shot as the default
   address plane: nine correctly-sized, non-blank, entirely wrong images. Fixed
   in Task 5, and Task 6 now has a pairwise-distinctness gate so the whole class
   of failure cannot recur silently, plus Step 4b to prove that gate bites.
2. The manifest's layer ids were checked against the real registry rather than
   assumed. All ten used (`terrain`, `exact`, `zoning`, `contours`, `canopy`,
   `opcode`, `access`, `relief`, `weather`, `ghost fog`) exist in
   `scene_layers_all()`; the full set also includes `confidence`, `statistical`,
   `edl`, `halos`, `vehicle`, `working set`, `lifetime`, `access order`,
   `sediment`, `convergence`, `crossings`, `taint`, `blame`, `ridge`.

**Verified against the real tree while planning:** `Workspace::open(path, err)`
returns `int`; `ShellState::scenes` is `std::vector<SceneView>` parallel to
`ws.recordings`; `ShellState::want_view_id` is `std::optional<ViewId>` and is
reset every frame (so it must be re-asserted); `LayerDesc::flag` is a
`bool SceneLayers::*` pointer-to-member; `scene_kind_name(LanePrism)` is exactly
`"SIMD lane prism"`; `cli/*.c` is inside the CI-gated `make fmt-check` while
`desktop/` is not.

**Unresolved at plan time, resolved during execution:** which Plane layers
populate from a recording with no `codeimage` stream. Task 4 Step 6 and Task 6
Step 4 surface this as a failing layer id or a blank image; the fix is a manifest
edit, not an architectural change.
