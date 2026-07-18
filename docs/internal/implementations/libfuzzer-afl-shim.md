# libFuzzer/AFL harness shim (Track E, demand-gated) — implementation

> **Sources.** Actioned from
> [post-v1-expansion-plan.md](../plans/post-v1-expansion-plan.md) — Track E
> ("Coverage-guided fuzzing & mutation testing"), **optional deliverable 3**:
> *"a libFuzzer/AFL harness shim exposing the emulator's coverage as the
> fuzzer's feedback channel, for users who want an external engine."* Written
> 2026-07-17. If this doc and a source disagree, this doc wins (sources may be
> stale); if the CODE and this doc disagree, re-verify before implementing.

## Why this work exists

The emulator already records basic-block coverage and already drives an
in-tree coverage-guided loop ([`emu_fuzz_cover1`](../../../src/fuzz.c#L33)) and a
mutation tester ([`emu_mutation_test1`](../../../src/fuzz.c#L111)). Those met
Track E's acceptance. This deliverable exposes that *same* coverage signal to an
**external** industrial fuzzer — libFuzzer or AFL++ — so a user can drive a guest
routine with a mature engine (its own corpus management, dictionaries, crash
minimization) instead of the in-tree loop.

The technical crux, and the whole reason this is a distinct skillset: **feed the
emulator's block coverage into the engine's feedback channel without
compiler-instrumenting the guest bytes.** The guest is raw x86-64 machine code
executed under Unicorn — `clang -fsanitize=fuzzer` and `afl-clang-fast` never see
it, so their automatic instrumentation covers only the harness loop. The shim
therefore registers an *external* coverage map and writes the emulator's executed
block offsets into it by hand, exactly as `afl-qemu-trace` / Unicorn-mode /
FRIDA-mode do for binary-only targets.

**This is demand-gated.** The plan explicitly leaves deliverable 3 "for concrete
demand — build only if the orchestrator prioritizes it," and the in-tree loop
already satisfies Track E. Task **T1** below (a small, host-independent
coverage-extraction seam) is worth landing regardless — it is the tested
foundation every harness stands on; tasks **T2–T5** are the external-engine shim
proper, built only when prioritized.

## What already exists (verified 2026-07-17)

- [src/fuzz.c](../../../src/fuzz.c) — a dependency-free translation unit that
  drives the emulator with the framework's seedable `splitmix64` RNG.
  `emu_fuzz_cover1` ([:33](../../../src/fuzz.c#L33)) keeps inputs that grow the
  block-coverage union; `emu_mutation_test1` ([:111](../../../src/fuzz.c#L111))
  bit-flips the routine and scores an input set. Both call
  [`emu_call_traced`](../../../include/asmtest_emu.h#L178) and add no dependency.
- [include/asmtest_emu.h](../../../include/asmtest_emu.h) — the emulator API.
  `emu_call_traced(e, fn, code_len, args, nargs, max_insns, out, trace)`
  ([:178](../../../include/asmtest_emu.h#L178)) runs a routine and records a
  trace; the Track E fuzz section is at
  [:634–707](../../../include/asmtest_emu.h#L634). `emu_trace_t` is a typedef of
  the engine-neutral `asmtest_trace_t`.
- [include/asmtest_trace.h](../../../include/asmtest_trace.h#L44) —
  `asmtest_trace_t`: `blocks[]` holds the **distinct** basic-block start offsets
  entered (deduped), `blocks_len` the count, `blocks_cap` the buffer size.
  Addresses are **byte offsets from routine entry** (offset 0 = entry), so every
  recorded block offset is strictly `< code_len`. That invariant is the load-
  bearing fact for indexing an external counter map (see T1).
- [examples/test_emu.c](../../../examples/test_emu.c) — the emulator suite. The
  Track E tests are at [:597–660](../../../examples/test_emu.c#L597). `CLASSIFY3`
  ([:585](../../../examples/test_emu.c#L585)) is a hand-assembled x86-64
  `classify(x) → {-1,0,+1}` with three branch paths; because it is raw bytes run
  under Unicorn it executes on **any** host arch (the coverage test at
  [:597](../../../examples/test_emu.c#L597) does *not* guard with
  `REQUIRE_X86_HOST`). It is the natural fixture for the shim's smoke tests.
- Build wiring: `make emu-test` ([Makefile:823](../../../Makefile#L823)) builds
  and runs `test_emu`; its link line
  ([Makefile:817–821](../../../Makefile#L817)) is
  `$(FRAMEWORK_OBJS) add.o mem.o flags.o branch.o emu.o trace.o disasm.o fuzz.o
  test_emu.o` linked with `$(UNICORN_LIBS) $(CAPSTONE_LIBS)`. `FRAMEWORK_OBJS`
  ([Makefile:52](../../../Makefile#L52)) is `asmtest.o capture.o` — it must be
  linked because `fuzz.o` references the `asmtest_rng_*` symbols. The `fuzz.o`
  rule is [Makefile:807](../../../Makefile#L807).
- Docker: `make docker-emu` ([mk/docker.mk:43](../../../mk/docker.mk#L43)) runs
  `emu-test` in an `ubuntu:24.04` image. The base [Dockerfile](../../../Dockerfile)
  installs only `build-essential` (gcc) plus the optional toolchain via
  `make deps` (`--emu` → libunicorn + libcapstone + pkg-config). **Neither `clang`
  nor `afl++` is present** — T4 adds them.
- Nothing matches `libFuzzer|LLVMFuzzer|afl` (word-boundary) anywhere in `src/`,
  `include/`, or `examples/` today, except a `.NET` example that only *illustrates*
  the AFL keep/discard decision conceptually. There is no C shim.

**Prove the baseline is green before touching anything.** On any host with Docker:

```sh
make docker-emu
# expected: builds ubuntu:24.04 + libunicorn + libcapstone, runs test_emu,
# every TEST line prints "ok", including the three Track E fuzz tests.
```

Or on a host with the deps: `make deps DEPS_ARGS=--emu && make emu-test`.

## Tasks

### T1 — `emu_cover_hits`: the tested coverage-extraction seam  (S, depends on: none)

**Goal.** One host-independent library function that runs a single input through
the emulator and returns the distinct executed block offsets, so every
external-engine harness bumps its coverage map through one tested seam rather than
re-deriving trace handling.

**Steps.**
1. Declare `emu_cover_hits` in [include/asmtest_emu.h](../../../include/asmtest_emu.h)
   in the Track E fuzz section, immediately after `emu_mutation_test1`
   (~[:688](../../../include/asmtest_emu.h#L688)).
2. Implement it in [src/fuzz.c](../../../src/fuzz.c) reusing
   `emu_call_traced` + a local `emu_trace_t` — no new dependency, staying in the
   same standalone TU.
3. Add a `TEST(emu, cover_hits_reports_executed_blocks)` to
   [examples/test_emu.c](../../../examples/test_emu.c) using `CLASSIFY3` (no
   `REQUIRE_X86_HOST` — it runs on every host under Unicorn).
4. Run `make emu-test` (and `make docker-emu DOCKER_PLATFORM=linux/arm64` to prove
   the arm64 lane).

**Code.** Signature (mirrors the `int nargs` and `uint64_t max_insns` of
`emu_call_traced`):

```c
/* Run `code` once on `args` inside the emulator and report the DISTINCT basic-
 * block byte-offsets it executed into caller-owned block_offs[0..cap) (deduped,
 * like the trace). Returns the count written (<= cap). Every offset is < code_len
 * (offsets are measured from routine entry), so a libFuzzer/AFL shim can size an
 * external counter array to code_len and index it directly by offset — no hash.
 * x86-64 guest, like the rest of Track E. */
size_t emu_cover_hits(emu_t *e, const void *code, size_t code_len,
                      const long *args, int nargs, uint64_t max_insns,
                      uint64_t *block_offs, size_t cap);
```

Body: zero a local `emu_trace_t`, point `trace.blocks = block_offs` and
`trace.blocks_cap = cap`, call `emu_call_traced(e, code, code_len, args, nargs,
max_insns ? max_insns : FUZZ_INSN_CAP, &r, &trace)`, then return `trace.blocks_len`.
Because the deduped block set is written straight into the caller's buffer there
is nothing to copy. Reuse the existing `FUZZ_INSN_CAP`
([src/fuzz.c:27](../../../src/fuzz.c#L27)) as the default cap so a runaway input
cannot spin.

**Tests.** In the new `TEST`, drive `CLASSIFY3` on three inputs and assert the
path-specific block appears among the returned offsets: `{-7}` reaches the
negative path (block at offset `0x12`), `{0}` the zero path (`0x11`), `{5}` the
positive path (`0x0c`); assert every returned offset is `< sizeof CLASSIFY3` and
the count is `>= 1`. A failure prints the assertion line with the wrong/absent
offset; a pass prints `ok` for the new TEST. This lane needs only libunicorn, so
it runs under `make emu-test` on x86-64 **and** arm64 — no external toolchain.

**Docs.** Internal-only for now (it is the seam, not a user surface); T5's
user-facing page documents it in context.

**Done when.**
- `make emu-test` prints the new TEST passing on x86-64.
- `make docker-emu DOCKER_PLATFORM=linux/arm64` prints it passing on arm64.
- `emu_cover_hits` returns a count `>= 1` and all offsets `< code_len` for a
  covered path.

### T2 — libFuzzer harness with externally-registered coverage  (M, depends on: T1)

**Goal.** `examples/fuzz_libfuzzer.c` that libFuzzer drives, feeding the guest's
block coverage into libFuzzer's feedback via `__sanitizer_cov_8bit_counters_init`.

**Steps.**
1. Create [examples/fuzz_libfuzzer.c](../../../examples/fuzz_libfuzzer.c). Embed a
   guest routine (start with a copy of `CLASSIFY3`) and its length as compile-time
   constants.
2. Add a `mk/fuzz.mk` (new target group, matching the `mk/*.mk` layout) and
   `include mk/fuzz.mk` in [Makefile](../../../Makefile#L858) next to the other
   includes. Add a `fuzz-libfuzzer` target that compiles the harness with
   `clang -fsanitize=fuzzer`.
3. Link the same object set `emu-test` uses **minus** the test file, plus the
   harness: `$(FRAMEWORK_OBJS) $(BUILD)/emu.o $(BUILD)/trace.o $(BUILD)/disasm.o
   $(BUILD)/fuzz.o` with `$(UNICORN_LIBS) $(CAPSTONE_LIBS)`. libFuzzer supplies
   `main`.
4. Smoke-run bounded (see Tests) and iterate.

**Code.** Harness shape (the load-bearing part is the counter registration):

```c
#include "asmtest_emu.h"
#include <stdint.h>
#include <string.h>

/* SanitizerCoverage callback: register an external 8-bit counter array so
 * libFuzzer treats our emulator coverage exactly like compiler-generated
 * counters. Declared by us because the guest is NOT clang-instrumented. */
extern void __sanitizer_cov_8bit_counters_init(uint8_t *start, uint8_t *stop);

static const uint8_t GUEST[] = { /* CLASSIFY3 bytes */ };
#define NCOUNTERS (sizeof GUEST)          /* block offsets are < code_len */
static uint8_t   g_counters[NCOUNTERS];
static uint64_t  g_blocks[NCOUNTERS];     /* deduped offsets fit in code_len slots */
static emu_t    *g_emu;

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc; (void)argv;
    g_emu = emu_open();
    __sanitizer_cov_8bit_counters_init(g_counters, g_counters + NCOUNTERS);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {
    memset(g_counters, 0, sizeof g_counters);   /* per-input coverage */
    long arg = 0;
    memcpy(&arg, Data, Size < sizeof arg ? Size : sizeof arg);
    size_t n = emu_cover_hits(g_emu, GUEST, sizeof GUEST, &arg, 1,
                              /*max_insns default*/0, g_blocks, NCOUNTERS);
    for (size_t i = 0; i < n; i++)
        if (g_blocks[i] < NCOUNTERS)
            g_counters[g_blocks[i]]++;         /* offset indexes the map directly */
    return 0;
}
```

Comment this for the junior reader: `-fsanitize=fuzzer` links libFuzzer's `main`
**and** its SanitizerCoverage runtime; the guest bytes are data (Unicorn runs
them), so no compiler counter exists for them — we register our own array and
libFuzzer consumes it as compiler-generated. The `memset` at the top of each
callback makes each input's coverage independent; libFuzzer reads the registered
region after the callback returns. This is exactly Jazzer's pattern (a non-C
runtime writing into a `__sanitizer_cov_8bit_counters_init` segment).

*Optional refinement:* also register a parallel PC table with
`__sanitizer_cov_pcs_init` mapping counter index → a synthetic PC (e.g.
`EMU_CODE_BASE + offset`) so a crash symbolizes to the guest offset. Per the
research caveat this is for symbolization only and is **not** required for the
feedback loop; add it if crash reports need guest-offset labels.

**Tests.** Two bounded runs, both wired into `fuzz-shim-test` (T4):
- *No-crash baseline:* the `CLASSIFY3` harness `-runs=50000 -max_len=8` exits 0 and
  its log shows `cov:` climbing (the guided search is finding the three paths).
- *Crash-finding:* build a second harness variant over a guest with a **reachable
  fault** (e.g. a routine that dereferences `[rdi]` only on the negative path, so
  a negative input faults). Assert `-runs=200000 -max_len=8` produces a
  `crash-*` artifact and exits non-zero within the budget — proving the coverage
  feedback actually steered the engine to the faulting path (a fixed vector would
  not reach it). A failure here is either "no crash found in budget" (coverage not
  wired) or a build/link error.

**Docs.** Documented in T5's user-facing page.

**Done when.**
- `make fuzz-libfuzzer` builds with `clang -fsanitize=fuzzer`.
- `./build/fuzz_libfuzzer -runs=50000` runs clean on `CLASSIFY3`.
- The crashing variant yields a `crash-*` artifact within budget.
- All of the above run inside `make docker-fuzz` (T4).

### T3 — AFL++ integration: aflpp_driver reuse + native forkserver  (M, depends on: T1, T2)

**Goal.** Drive the same coverage seam under AFL++, two ways: the low-effort
reuse of T2's harness, and a libFuzzer-free native harness.

**Steps / Code.**

*Path A (recommended) — reuse T2's harness under AFL++ via `aflpp_driver`.*
AFL++ ships `libAFLDriver.a`, which bridges `LLVMFuzzerTestOneInput` into AFL++'s
persistent-mode + shared-memory testcase loop and consumes the counters registered
by `__sanitizer_cov_8bit_counters_init`. Build target `fuzz-afl-driver`:

```sh
afl-clang-fast++ -fsanitize=fuzzer examples/fuzz_libfuzzer.c \
    build/emu.o build/trace.o build/disasm.o build/fuzz.o \
    build/asmtest.o build/capture.o -lunicorn -lcapstone -o build/fuzz_afl_driver
```

No new harness file — the T2 harness serves both engines. (`aflpp_driver` is
documented on a third-party deepwiki mirror, not the official AFL++ docs; verify
the exact flag against the installed AFL++ once pinned — see Research notes.)

*Path B — native persistent-mode forkserver harness (no libFuzzer dependency).*
Create [examples/fuzz_afl.c](../../../examples/fuzz_afl.c) using the AFL++ macros
and writing **node** coverage into AFL's shared-memory bitmap by hand:

```c
#include "asmtest_emu.h"
#include <stdint.h>
#include <string.h>

__AFL_FUZZ_INIT();                         /* file scope */
extern uint8_t *__afl_area_ptr;            /* the shm bitmap (afl-clang-fast) */
#define MAP_SIZE 65536                     /* AFL++ default (1<<16) */

static const uint8_t GUEST[] = { /* CLASSIFY3 bytes */ };
static uint64_t g_blocks[sizeof GUEST];

int main(void) {
    emu_t *e = emu_open();
    __AFL_INIT();                          /* deferred forkserver, after emu_open */
    unsigned char *buf = __AFL_FUZZ_TESTCASE_BUF;
    while (__AFL_LOOP(10000)) {
        int len = __AFL_FUZZ_TESTCASE_LEN;
        long arg = 0;
        memcpy(&arg, buf, (size_t)len < sizeof arg ? (size_t)len : sizeof arg);
        size_t n = emu_cover_hits(e, GUEST, sizeof GUEST, &arg, 1, 0,
                                  g_blocks, sizeof GUEST);
        for (size_t i = 0; i < n; i++) {
            uint64_t off = g_blocks[i];
            /* binary-only/emulator block hash (lcamtuf): derive a location from
             * the block address, mask to the map. */
            uint32_t loc = (uint32_t)(((off >> 4) ^ (off << 8)) & (MAP_SIZE - 1));
            __afl_area_ptr[loc]++;
        }
    }
    return 0;
}
```

Compile with `afl-clang-fast` (target `fuzz-afl`). Explain for the reader:
`afl-clang-fast` instruments only the harness's own code (a small, constant
background in the map); the guest runs under Unicorn, invisible to it, so we write
its block hits into `__afl_area_ptr` ourselves — the `afl-qemu-trace` /
Unicorn-mode mechanism. `__AFL_INIT()` starts the forkserver *after* `emu_open()`
(deferred forkserver: expensive setup runs once); `__AFL_LOOP(N)` gives
persistent-mode reuse (~2×–20×); the testcase arrives in shm via
`__AFL_FUZZ_TESTCASE_BUF` / `_LEN`.

*Scope note:* this exports **node** (per-block) coverage, matching the libFuzzer
8-bit-counter model. AFL's native model is **edge** coverage
(`map[cur ^ prev]++; prev = cur >> 1`), which needs the *ordered* block sequence;
`asmtest_trace_t.blocks` is deduped, so edge coverage would need a new ordered-block
accessor and is **out of scope** (see Out of scope).

**Tests.**
- *No-engine replay smoke (Path B, no afl-fuzz needed):* run outside `afl-fuzz`,
  where `__AFL_LOOP` executes once reading stdin. `printf '\xf9\xff\xff\xff' |
  ./build/fuzz_afl` exits 0 — a deterministic unit check that the harness builds
  and runs one iteration. Wire this into `fuzz-shim-test`.
- *Under afl-fuzz (crashing guest):* `afl-fuzz` aborts at startup when the `-i`
  directory is missing or holds no seed file, so create and seed it first —
  `mkdir -p seeds && printf '\x05\x00\x00\x00' > seeds/a` — then `afl-fuzz -i seeds
  -o out -V 15 -- ./build/fuzz_afl` (or `./build/fuzz_afl_driver`) reaches
  `out/default/crashes/` within the 15-second budget on the crashing-guest variant;
  on `CLASSIFY3` it just accumulates paths. `fuzz-shim-test` (T4) performs this same
  seed setup in-lane before invoking `afl-fuzz`. In a container set
  `AFL_SKIP_CPUFREQ=1` and `AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES=1` so `afl-fuzz`
  does not abort on the runner's cpufreq/core-pattern settings.

**Docs.** Documented in T5's user-facing page.

**Done when.**
- `make fuzz-afl` and `make fuzz-afl-driver` build.
- The single-input replay exits 0.
- A 15 s `afl-fuzz -V` run on the crashing guest yields `>= 1` entry in
  `crashes/`; both run inside `make docker-fuzz`.

### T4 — Toolchain lane + version pins + CI smoke  (M, depends on: T2, T3)

**Goal.** A `Dockerfile.fuzz` + `docker-fuzz` lane that installs `clang` + `afl++`
and runs bounded smokes for both engines, so the shim is **genuinely tested
in-lane, never self-skipped** — `clang` and `afl++` are installable, so per the
project's CLAUDE.md dependency rule they are *added where the work runs*, not
gated away.

**Steps.**
1. Add [Dockerfile.fuzz](../../../Dockerfile.fuzz): `FROM asmtest-bindings-base`
   (it already carries libunicorn + libcapstone), then
   `apt-get install -y --no-install-recommends clang afl++`. If
   `clang -fsanitize=fuzzer` cannot find the fuzzer runtime, also install the
   matching `libclang-rt-<ver>-dev` (Ubuntu 24.04 ships clang 18). Record the
   apt-resolved `clang` and `afl++` versions in a header comment.
2. Add `docker-fuzz` to [mk/docker.mk](../../../mk/docker.mk). Mirror the
   `docker-win64` ([mk/win64.mk:221](../../../mk/win64.mk#L221)) /
   `docker-cli` ([mk/cli.mk:364](../../../mk/cli.mk#L364)) shape, **not**
   `docker-emu` ([mk/docker.mk:43](../../../mk/docker.mk#L43)): every
   FROM-bindings-base target declares a `docker-bindings-base` prerequisite so the
   base image exists before the Dockerfile consumes it, whereas
   `docker-emu: docker-build` only builds the plain root Dockerfile and would leave
   `asmtest-bindings-base` unbuilt. Because `Dockerfile.fuzz` is
   `FROM asmtest-bindings-base` (step 1), write `docker-fuzz: docker-bindings-base`,
   then `$(DOCKER) build $(_docker_plat) -f Dockerfile.fuzz -t asmtest-fuzz .`
   followed by `$(DOCKER) run --rm $(_docker_plat) asmtest-fuzz make
   fuzz-shim-test`. Pass `-e AFL_SKIP_CPUFREQ=1
   -e AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES=1` on the `docker run` for the afl-fuzz
   step.
3. Add `fuzz-shim-test` to `mk/fuzz.mk`: build every harness, run the libFuzzer
   `-runs` baseline + the crashing-guest run, the AFL single-input replay, and a
   short `afl-fuzz -V 15` on the crashing guest (creating and seeding the `-i` input
   dir first, since `afl-fuzz` aborts on a missing or empty seed dir); assert each
   engine finds its planted crash (fail the target otherwise).
4. Add `make help` lines for `fuzz-libfuzzer`, `fuzz-afl`, `fuzz-shim-test` and a
   docker help line for `docker-fuzz`.

**Tests.** `make docker-fuzz` *is* the test: it must exit 0 and print the
planted-crash discovery lines for both engines. A regression looks like a non-zero
exit with "no crash found in budget" (coverage feedback broke) or a build error
(dependency drift).

**Docs.** Covered by T5.

**Done when.**
- `make docker-fuzz` is green on an x86-64 host and prints the discovered crash for
  both engines.
- The image comment records the pinned `clang` / `afl++` versions.
- No self-skip exists in the lane — these are installable dependencies, not a
  hardware/credential gate.

### T5 — Docs + changelog  (S, depends on: T2, T3, T4)

**Goal.** A user-facing guide page plus changelog and cross-links.

**Steps.**
1. Add `docs/guides/fuzzing-shim.md` (Sphinx; the docs build is `-W`
   fail-on-warning, so no broken references) covering: when to use the external
   shim vs. the in-tree `emu_fuzz_cover1`; the "coverage without guest
   instrumentation" mechanism; the libFuzzer build/run recipe; the AFL++
   aflpp_driver and native-forkserver recipes; and the node-vs-edge scope note.
2. Add it to the guides toctree so it publishes.
3. Cross-link from the fuzzing section of
   [docs/guides/emulator.md](../../guides/emulator.md#L325) ("for an external
   engine, see the fuzzing-shim guide").
4. Append one `### Added` bullet under `## [Unreleased]` in
   [CHANGELOG.md](../../../CHANGELOG.md).

**Docs.** This task *is* the docs.

**Done when.**
- `make docs` (or `make docker-docs`) builds `-W`-clean with the new page in the
  toctree.
- `CHANGELOG.md` has one `Added` bullet describing the shim.

## Task order & parallelism

- **T1** is independent and lands first — the tested seam, valuable even if the
  shim is deprioritized. It needs only libunicorn and runs on every host.
- **T2** and **T3 Path B** depend only on T1 and touch different files (different
  engines), so two people can work them concurrently.
- **T3 Path A** reuses T2's harness file, so it follows T2.
- **T4** runs everything T2/T3 produce, so it follows both.
- **T5** documents the finished shim, last.

Critical path: **T1 → T2 → T3(A) → T4 → T5**. Off the critical path: T3 Path B may
proceed in parallel with T2.

## Constraints & gates

- **Demand gate.** The external shim (T2–T5) is optional deliverable 3 of Track E;
  build it only if the orchestrator prioritizes it. T1 is worth landing regardless
  as the tested seam.
- **No hardware/credential gate.** `clang` and `afl++` are installable, so per
  CLAUDE.md they are added to the Docker lane (T4) and the shim is tested for real —
  never self-skipped. The only legitimate self-skips (a specific CPU generation,
  Intel PT, Apple silicon, credentials) do not apply here.
- **Pinning.** [scripts/third-party-digests.txt](../../../scripts/third-party-digests.txt)
  pins only DynamoRIO 11.91.20630, keystone 0.9.2, capstone 5.0.1, and zig
  0.13.0 — there is **no** libFuzzer/AFL++ pin, and none is needed: that file
  anchors tarball/git-source fetches, whereas `clang` and `afl++` come from apt and
  are version-pinned by the `ubuntu:24.04` base image. No new digest line is
  required. If a future version switches AFL++ to a source build, add a pinned ARG
  + digest at that point, following the `Dockerfile.drtrace` / `build-capstone.sh`
  pattern.
- **License.** libFuzzer ships with LLVM (Apache-2.0 with the LLVM exception);
  AFL++ is Apache-2.0. Both are *build/test tooling* here — they are never bundled
  into a user-facing package (unlike Keystone/Capstone, which ship inside
  `libasmtest_emu_full`), so there is no packaging-license constraint.
- **What to record if blocked.** If the orchestrator does *not* prioritize the
  shim, record T1 as landed (the seam) and T2–T5 as deferred-on-demand, with a
  pointer to this doc — do not leave a half-wired harness that can only self-skip.

## Research notes (verified 2026-07-17)

No repo pin exists for these engines, so the facts below track current upstream
(LLVM/clang, AFL++ stable) and must be re-checked against the installed versions
once T4 pins them.

- **libFuzzer entry points.** Harness:
  `extern "C" int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size)`;
  optional `int LLVMFuzzerInitialize(int *argc, char ***argv)`. `clang
  -fsanitize=fuzzer` instruments **and** links the libFuzzer runtime (supplies
  `main`); `-fsanitize=fuzzer-no-link` adds instrumentation only.
  [llvm.org/docs/LibFuzzer.html](https://llvm.org/docs/LibFuzzer.html)
- **SanitizerCoverage external counters.**
  `extern "C" void __sanitizer_cov_8bit_counters_init(char *start, char *end)`
  registers an 8-bit counter array; `__sanitizer_cov_pcs_init(pcs_beg, pcs_end)` a
  parallel `[PC, PCFlags]` table (for symbolization). The T2 harness spells the
  extern `uint8_t *start, uint8_t *stop` to match its `uint8_t g_counters[]` array;
  the `char*` vs `uint8_t*` difference is immaterial for the extern (both are byte
  pointers and C resolves the call purely by symbol name), so both declarations name
  the same runtime function.
  [clang.llvm.org/docs/SanitizerCoverage.html](https://clang.llvm.org/docs/SanitizerCoverage.html)
- **Feeding an external map (Jazzer pattern).** A non-C runtime writes coverage
  into a segment registered via `__sanitizer_cov_8bit_counters_init`; libFuzzer
  consumes it as compiler-generated. This is the exact model the shim uses for
  Unicorn-executed guest bytes.
  [github.com/CodeIntelligenceTesting/jazzer/discussions/483](https://github.com/CodeIntelligenceTesting/jazzer/discussions/483)
- **AFL++ shared-memory bitmap.** `__AFL_SHM_ID` names the shm segment;
  instrumentation writes through `__afl_area_ptr`; default `MAP_SIZE = 65536`
  (`1<<16`), overridable by `AFL_MAP_SIZE`. Binary-only/emulator location:
  `cur_location = (block_address >> 4) ^ (block_address << 8)`, then edge index
  `map[cur ^ prev]++; prev = cur >> 1`.
  [lcamtuf.coredump.cx/afl/technical_details.txt](https://lcamtuf.coredump.cx/afl/technical_details.txt),
  [aflplus.plus/docs/technical_details/](https://aflplus.plus/docs/technical_details/),
  [env_variables.md](https://github.com/AFLplusplus/AFLplusplus/blob/stable/docs/env_variables.md)
- **AFL++ persistent mode / deferred forkserver.** `__AFL_FUZZ_INIT();` (file
  scope), `__AFL_INIT();` (after expensive setup), `while (__AFL_LOOP(N)) { … }`,
  testcase via `__AFL_FUZZ_TESTCASE_BUF` / `__AFL_FUZZ_TESTCASE_LEN`; compile with
  `afl-clang-fast`. ~2×–20× speedup.
  [aflplus.plus/docs/fuzzing_in_depth/](https://aflplus.plus/docs/fuzzing_in_depth/)
- **aflpp_driver.** A libFuzzer harness runs under AFL++ via `libAFLDriver.a`:
  `afl-clang-fast++ -fsanitize=fuzzer harness.cpp target.a`. Documented on a
  third-party deepwiki mirror of the repo, **not** the official docs, so verify the
  flag against the installed AFL++.
  [deepwiki.com/AFLplusplus/AFLplusplus/6.4-libfuzzer-compatibility-and-aflpp_driver](https://deepwiki.com/AFLplusplus/AFLplusplus/6.4-libfuzzer-compatibility-and-aflpp_driver)
- **Caveats carried forward.** `__sanitizer_cov_pcs_init` is for symbolization, not
  strictly required for the feedback loop; the AFL++ custom-mutator prototypes vary
  by version (not used by this shim); no repo pin exists for either engine today.

## Out of scope

- **The in-tree loop and mutation tester** (`emu_fuzz_cover1` /
  `emu_mutation_test1`, [src/fuzz.c](../../../src/fuzz.c)) already shipped Track E's
  acceptance — this doc *consumes* them via `emu_call_traced` and does not change
  them.
- **AFL++ custom-mutator API** (`afl_custom_fuzz` and siblings) — that integrates a
  mutator *into the engine*, a different feature from exporting coverage; available
  but not part of this shim.
- **Edge (vs node) coverage** — AFL's native `map[cur ^ prev]` model needs the
  ordered block sequence; `asmtest_trace_t.blocks` is deduped, so edge coverage
  would need a new ordered-block accessor. Node coverage is the baseline here.
- **Non-x86-64 guests.** The fuzz seam is x86-64-guest-only, like
  `emu_fuzz_cover1`; arm64/RISC-V/ARM32 guests are not fuzzed by this shim.
- **Sibling implementation docs.** No sibling in this doc set owns the fuzzer shim;
  it is self-contained in `src/fuzz.c` + a new `examples/fuzz_*.c` + `mk/fuzz.mk` +
  `Dockerfile.fuzz`. The emulator-tier siblings
  ([asmspy-cli-enhancements.md](asmspy-cli-enhancements.md) and the dataflow docs)
  cover unrelated tiers and are not restated here.
