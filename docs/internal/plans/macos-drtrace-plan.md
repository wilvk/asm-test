# asm-test — DynamoRIO native-trace tier: macOS port plan

A phased roadmap for porting the DynamoRIO in-process native-trace tier
(`libasmtest_drapp` + `libasmtest_drclient`) from Linux x86-64 to macOS, covering
both the Intel x86-64 (Rosetta-capable) and Apple Silicon arm64 targets.

The DynamoRIO tier (Phases 0–7) is **implemented and validated** on Linux x86-64.
macOS was explicitly deferred in the
[DynamoRIO native-trace plan](../archive/plans/dynamorio-native-trace-plan.md): "Start Linux x86-64
only; treat macOS/AArch64 as follow-up." This plan is that follow-up.

Hardware-assisted tracing (Intel PT / CoreSight) is **not** the subject of this
plan and remains unavailable on macOS: macOS exposes no `perf_event_open` AUX
interface, and Apple Silicon locks ETM out of userspace entirely. Only the
DynamoRIO-based tier is addressed here.

> Status legend: **planned** unless noted. Update as phases land.

> **STATUS 2026-07-16 — 0 of 3 phases landed. BLOCKED UPSTREAM, not on hardware.**
>
> **Buying a Mac does not unblock this plan.** The gate is that DynamoRIO has
> **never published a macOS release asset**, so there is nothing to attach *with*
> — Apple hardware, on its own, buys you nothing here. Do not schedule M0/M1/M2,
> and do not treat "get a Mac" as the unblocking step. The only avenue is a
> from-source macOS DR build (out of scope; see below).
>
> **Before anyone touches this plan, re-run the upstream check** (it is one
> command and it is the whole go/no-go):
>
> ```
> gh api --paginate '/repos/DynamoRIO/dynamorio/releases' --jq '.[].assets[].name' \
>   | grep -iE 'mac|darwin|osx' || echo 'still no macOS asset — plan stays blocked'
> ```
>
> Nothing else in this document matters until that command prints an asset.

> **Phase M0 status (2026-06-30): BLOCKED UPSTREAM — no-go via the documented path.**
> The Step-0 prerequisite — an official macOS DynamoRIO *release* — **does not
> exist and never has.** A scan of the DynamoRIO GitHub releases API across **all
> ~295 releases** (cronbuilds + stable tags back to `8.0.x`) returns only four
> asset platforms — `DynamoRIO-AArch64-Linux`, `DynamoRIO-ARM-Linux-EABIHF`,
> `DynamoRIO-Linux` (x86-64), and `DynamoRIO-Windows` — and **zero**
> macOS/Mac/Darwin assets. DynamoRIO's own README still reads *"Mac OSX support is
> in progress"* and its [releases page](https://dynamorio.org/page_releases.html)
> lists only the Windows + Linux variants. Per Step 0's own instruction ("if the
> macOS tarball does not exist … **stop** — the port is blocked upstream") this
> phase cannot proceed on a prebuilt release. The only remaining avenue is a
> **from-source macOS build of DynamoRIO** (out of scope here; "in progress"
> upstream, with the `dr_app_*` all-thread Mach takeover historically its weakest
> path), so M1/M2 stay **held** as the plan directs. See [Phase M0 → Step 0
> result](#phase-m0--x86-64-attach--compiled-function-tracing-planned) below. The
> Linux Phase 1–7 tier is unaffected.

> **Re-confirmed 2026-07-16 — unchanged, on a wider scan. Still NO-GO.**
> Re-ran the releases-API query against **all 454 releases** (the 2026-06-30 scan
> saw ~295; latest cronbuild is now `11.91.20644`, was `11.91.20630`). Result:
> **still zero macOS/Mac/Darwin/dylib assets, in any release, ever.** Upstream's
> README is also unchanged — *"IA-32, AMD64, ARM, and AArch64 hardware. Mac OSX
> support is in progress."*
>
> One correction to the 2026-06-30 block above, which does not affect its
> conclusion: that scan's "only four asset platforms" was an **under-count** of the
> enumeration (not of macOS). The full set across 454 releases is **seven**
> platform prefixes — `DynamoRIO-Windows` (441), `DynamoRIO-Linux` (351),
> `DynamoRIO-ARM-Linux-EABIHF` (346), `DynamoRIO-AArch64-Linux` (344),
> `DynamoRIO-ARM-Android-EABI` (283), `DynamoRIO-i386-Linux` (98), and
> `DynamoRIO-x86_64-Linux` (95) — plus some tutorial PDFs. The load-bearing fact is
> the same and is now established over a broader sample: **macOS is not among
> them.** M0 stays NO-GO; M1/M2 stay held.

> **Re-confirmed 2026-07-20 — unchanged. Still NO-GO.**
> Re-ran the releases-API query across **all 455 releases** (was 454 on
> 2026-07-16; latest cronbuild is now `cronbuild-11.91.20651`, was
> `11.91.20644`). Result: **still zero macOS/Mac/Darwin/dylib assets, in any
> release, ever** — `gh api --paginate '/repos/DynamoRIO/dynamorio/releases'
> --jq '.[].assets[].name' | grep -icE 'mac|darwin|osx|dylib'` → **0**. The
> published platform set is unchanged but for one negligible legacy addition:
> `DynamoRIO-Windows` (442), `DynamoRIO-Linux` (352),
> `DynamoRIO-ARM-Linux-EABIHF` (347), `DynamoRIO-AArch64-Linux` (345),
> `DynamoRIO-ARM-Android-EABI` (283), `DynamoRIO-i386-Linux` (98),
> `DynamoRIO-x86_64-Linux` (95), and a single `DynamoRIO-ARM-Linux-EABI` (1) —
> all Windows / Linux / Android, **no macOS**. M0 stays NO-GO; M1/M2 stay held,
> and buying Apple hardware still does not unblock the plan.
>
> **Recheck cadence (kept manual):** re-run the one-command gate above **before
> any M0/M1/M2 work is ever scheduled**, and whenever this plan or the
> `macos-dynamorio-port` implementation doc is next touched. A scheduled CI job
> that "fails successfully" for years is noise, not signal — a one-command check
> does not warrant automation.

> **STATUS 2026-07-22 — GATE CHANGED: unblocked via a source fork. The recheck
> above is no longer the go/no-go.** A local fork of DynamoRIO
> (`https://github.com/wilvk/dynamorio`, at `/Users/willemvanketwich/source/dynamorio`,
> upstream master HEAD `cca42665b`) **builds `lib64/release/libdynamorio.dylib`
> on macOS x86-64** — verified live on the Intel-mac dev host (macOS 14.7.5,
> Apple clang 16, CMake 4.3.4; submodules `elfutils`/`libipt`/`zlib` init'd; deps
> `nasm`+`lz4`+SDK `zlib`). So there is now a runtime to attach *with*; the
> "buying a Mac buys nothing" framing above is superseded. The from-source path —
> previously called out-of-scope — is now the scoped avenue:
> [macos-dynamorio-fork-build.md](../implementations/macos-dynamorio-fork-build.md)
> (FB1 pins the fork + adds `scripts/build-dynamorio-macos.sh`; FB2 fixes the M0
> blocker; FB3 drives T3/T4/T5; FB4 adds the nightly CI lane).
>
> **The dominant risk below is now partly measured.** Upstream CI-gates
> `api.startstop`/`api.detach` (the `dr_app_*` interface) on `macos-15-intel` —
> they are **not** in the `$is_macos` ignore-failures list in
> `suite/runsuite_wrapper.pl` (Windows ignores `api.startstop`; macOS does not),
> so upstream expects that path to pass on macOS x86-64. On the dev host it runs
> the full 10-iteration attach/detach cycle correctly **but** faults at
> `dr_app_setup` startup in `get_application_name_helper` (`core/unix/os.c:1192`,
> an exec-path-above-`envp` deref, stack-layout dependent). So M0's bar — "does
> in-process attach work on this macOS/DR combination" — is **provisionally
> cleared for the mechanism, pending FB2's fix of that startup crash.** M1/M2
> unchanged.

> **STATUS 2026-07-22 (later) — M0 = GO. Landed and measured on the Intel-mac
> dev host (macOS 14.7.5).** FB1–FB3 of
> [macos-dynamorio-fork-build.md](../implementations/macos-dynamorio-fork-build.md)
> are done: the fork is pinned + script-built (`make dynamorio-macos`), and
> **three** root-caused fixes in the fork (branch `asmtest/macos-fixes`, pin
> `bbbcc40b8`) made the `dr_app_*` embedding functional — (1) the
> `get_application_name_helper` startup fault (exec path now read from
> `KERN_PROCARGS2`; standalone baseline was 10/10 SIGSEGV, after: 10/10 clean);
> (2) `our_environ` was NULL in the dlopen embedding (dyld passes no envp to
> dylib initializers), so `DYNAMORIO_OPTIONS` was invisible and **no client
> ever loaded** — now captured live via `_NSGetEnviron` (the STATIC_LIBRARY
> approach); (3) `dr_get_proc_address` returned NULL on every modern binary —
> `LC_DYLD_EXPORTS_TRIE` was unparsed, and the trie rebase used the ASLR slide,
> correct only for preferred-base-0 dylibs, never for the main executable.
> With those, port-doc **T3/T4/T5 are landed** and the M0 harness
> (`make drtrace-test-macos`) traces a normally-compiled `__TEXT` function end
> to end — attach, **Mach-O marker resolution via `dr_get_proc_address` (the
> load-bearing unknown: it resolves)**, coverage accumulation, symbol mode,
> clean detach — **13/13, twice in a row**, against the pinned script-built
> home. **One premise above is corrected by measurement:** upstream does NOT
> CI-gate `api.startstop`/`api.detach` on macOS — its macOS suite runs only
> tests labeled `OSX` (`INCLUDE_LABEL OSX`, i#1815) and those two are not in
> the label set, so their absence from the ignore list is vacuous; their
> multi-thread takeover assertions are unpassable in-tree anyway
> (`os_list_threads`/`thread_signal` NYI on macOS, i#58). The M0/M1 tier is
> single-threaded and unaffected. Next: FB4 (nightly `drtrace-macos` CI lane),
> then M1 (generated-bytes W^X path) / M2 per this plan.

> **STATUS 2026-07-22 (M2 bindings,
> [macos-dynamorio-port.md](../implementations/macos-dynamorio-port.md) T9) —
> lanes green on macOS x86-64, with one NEW measured limitation.**
> `drtrace_env` and the `drtrace-python-test` env block now resolve
> platform-correct names (`shlib_dev` → `.dylib`, `$(DR_CLIENT)`, and
> `DYLD_LIBRARY_PATH` on Darwin via the new `DR_LOADER_PATH` var); no wrapper
> needed source fixes (zig/python already carry Darwin-correct fallbacks; all
> wrappers honor the env-var contract). Live on the Intel-mac dev host:
> `drtrace-cpp-test` PASS, `drtrace-ruby-test` PASS, `drtrace-python-test`
> 3+3 passed / 1 skipped. **The skip is a real finding: DR signal chaining
> HANGS on the macOS fork build.** `test_drgate.py::test_signal_chaining`
> (SIGUSR1 self-raised while attached; the client answers
> `DR_SIGNAL_DELIVER`) never delivers to CPython's handler and wedges the
> process — exactly watch-item (5) of the M0 checklist, which the
> single-threaded C harnesses never exercise. Skipped on Darwin only, with
> this reason string, in `bindings/python/tests/test_drgate.py`; Linux keeps
> running it. Root-causing DR's macOS signal delivery is fork-side work
> (i#58-adjacent, sibling of the three FB2 fixes), out of T9's bindings
> scope — recorded here so it is a named limitation, not a silent one.

## The dominant risk, stated first

**The single largest unknown is whether DynamoRIO's Application Interface
(`dr_app_setup`/`dr_app_start`/…) works on macOS at all.** DynamoRIO's macOS
port is its least-maintained target. The most likely outcome of the first spike
is *not* "a few days of plumbing" but **"in-process attach does not work on this
macOS/DR combination, full stop."** Everything else in this plan is contingent
on Phase M0 clearing that bar. Read the effort estimates as "if it works"; the
binary question of whether it works dominates them.

Two pieces of the existing design are the most exposed on macOS:

- **`dr_app_start` all-thread takeover.** On Linux this enumerates the thread
  group and signals each thread; on macOS it must use Mach thread primitives.
  This path has historically been the weakest part of DR's macOS support.
- **Marker resolution via `dr_get_proc_address` against a Mach-O main executable.**
  The *entire* begin/end/region mechanism depends on the client resolving the
  exported marker functions (`asmtest_trace_begin`, `asmtest_dr_register_region_marker`,
  …) by name in the main module — see
  [drtrace_client.c:331-341](../../../src/drtrace_client.c#L331-L341). On Linux this
  is why `test_drtrace` links `-rdynamic` (to populate `.dynsym`). Whether the
  Mach-O equivalent makes those symbols visible to `dr_get_proc_address` is
  unverified and **load-bearing** — if it fails, M0 fails regardless of anything
  else.

---

## Tracing compiled code vs. generated code — the key distinction

The tier has two ways to get traceable code, and they have **completely different**
macOS difficulty. The original (Linux) plan never had to separate them; on macOS
they must be separated because they hit different OS restrictions.

| Path | API | Executable memory? | macOS arm64 cost |
|---|---|---|---|
| **Compiled function** | `asmtest_dr_register_region` over a normal C function, or `asmtest_dr_register_symbol` | No — the code is already in the binary's `__TEXT` | **None.** No W^X, no `MAP_JIT`, no entitlement. Arch-agnostic. |
| **Generated bytes** | `asmtest_exec_alloc` / `asmtest_asm_exec_native` | Yes — `mmap`+`mprotect` an executable page | **High.** Apple Silicon W^X requires `MAP_JIT` + the JIT entitlement on the **main executable** (see M1). |

The Linux smoke test ([examples/test_drtrace.c](../../../examples/test_drtrace.c))
leans almost entirely on the **generated-bytes** path: every traced region is built
with `asmtest_exec_alloc()` over a **hardcoded x86-64 machine-code** `ROUTINE[]`
([test_drtrace.c:47-54](../../../examples/test_drtrace.c#L47-L54)). Only the
symbol-mode case ([test_drtrace.c:154-161](../../../examples/test_drtrace.c#L154-L161),
`asmtest_symbol_demo`) traces a normally-compiled function.

Two consequences drive this plan's structure:

1. The **compiled-function path is the macOS happy path** and is what M0 should
   prove. It needs no executable-memory machinery and therefore sidesteps every
   W^X/entitlement problem on both arches.
2. The existing smoke test **cannot run on arm64 as written** — `ROUTINE[]` is
   x86-64 bytes that SIGILL when called as an arm64 function. Any arm64 acceptance
   needs either a new arm64 byte fixture or a pivot to symbol/compiled-function
   mode (M1 owns this).

---

## What already works on macOS

- **Unicorn emulator tier** (`make emu-test`): fully functional, 47 tests pass.
- **`drtrace_app.c` compilation**: verified — `cc -c src/drtrace_app.c` succeeds
  on macOS x86-64 (all POSIX APIs it uses — `dlopen`, `pthread`, `mmap`,
  `mprotect` — exist on Darwin). The object is dead-linked today because the
  runtime tier is not wired.
- **`hwtrace-test`**: builds and self-skips cleanly (`1..0 # skipped`).

**Dev-machine note.** This repo's current macOS host reports `uname -m` =
`x86_64` (an Intel Mac, or x86-64 shell). M0 (x86-64) is therefore directly
testable here; M1 (arm64 W^X) is **not exercisable on this machine** and is gated
on separate Apple Silicon hardware from day one.

## Concrete blockers (mechanical)

1. **Library extension.** `dr_lib_path()` in
   [drtrace_app.c:51-71](../../../src/drtrace_app.c#L51-L71) and `DR_DLLIB` in the
   Makefile both hardcode `libdynamorio.so`; macOS is `libdynamorio.dylib`.
2. **macOS DR lib directory layout unverified.** Linux uses
   `$DYNAMORIO_HOME/lib64/release/`; the macOS release layout must be confirmed
   against an actual tarball before the path is wired.
3. **Client library name is `.so` everywhere.** The Makefile target, the
   `ASMTEST_DRCLIENT` env var, and the per-language lanes all say `.so`; CMake
   emits `.dylib` on macOS.
4. **`dr_get_proc_address` marker resolution on Mach-O** — load-bearing, see the
   dominant-risk section. The Linux mechanism is `-rdynamic`; the Mach-O behaviour
   must be proven in M0.
5. **Generated-code W^X on arm64** — only affects the generated-bytes path; see M1.

Minor, non-blocking: `-ldl` is a no-op on macOS (dlopen lives in libSystem);
`-lpthread` is in libSystem; `-rdynamic` is accepted by macOS clang (maps to
`-export_dynamic`) but its effect on `dr_get_proc_address` resolution is the part
that matters (blocker 4, not cosmetics).

---

## Phase M0 — x86-64 attach + compiled-function tracing *(planned)*

**Goal.** Prove that in-process DynamoRIO attach **and** the marker/region
mechanism work on macOS x86-64, tracing a **normally-compiled** function. No
generated executable memory is involved, so this phase is free of every
W^X/entitlement concern and isolates the two real unknowns: does `dr_app_*`
attach work, and does `dr_get_proc_address` resolve markers in a Mach-O
executable.

This is the go/no-go for the whole port.

**Step 0 — Obtain and inspect a macOS DynamoRIO release.**

Download the official macOS tarball from DynamoRIO's GitHub releases
(naming convention `DynamoRIO-MacOS-<ver>.tar.gz`). Before writing code, verify
against the unpacked tree and **record the findings in this doc**:

- The actual path of `libdynamorio.dylib` relative to the root (expected
  `lib64/release/`, but confirm — the macOS layout may differ).
- A `cmake/` dir with `DynamoRIOConfig.cmake` and a working
  `configure_DynamoRIO_client` (needed to build the client).
- `lipo -info libdynamorio.dylib` — is it x86-64-only, or universal (also arm64)?
  This determines whether M1 needs a separate arm64 download.

If the macOS tarball does not exist or does not ship the Application Interface
+ client CMake support, **stop** — the port is blocked upstream.

> **Step 0 result — recorded 2026-06-30: the macOS tarball does not exist.**
> Queried the DynamoRIO GitHub releases API
> (`/repos/DynamoRIO/dynamorio/releases`) across every page. The complete set of
> release-asset platform prefixes, over **all ~295 releases** (latest cronbuild at
> the time was `11.91.20630`; stable tags back to `8.0.x`), is exactly:
>
> | Asset prefix | Count (approx.) |
> |---|---|
> | `DynamoRIO-AArch64-Linux-` | ~295 |
> | `DynamoRIO-ARM-Linux-EABIHF-` | ~295 |
> | `DynamoRIO-Linux-` (x86-64) | ~295 |
> | `DynamoRIO-Windows-` | ~293 |
> | `DynamoRIO-MacOS-` / `-Mac-` / `-Darwin-` | **0** |
>
> There is **no** macOS/Mac/Darwin release asset, in any release, ever.
> *(Re-confirmed 2026-07-16 over all 454 releases — see the re-confirmation block
> at the top of this doc, which also corrects this table's prefix enumeration.)*
> The `DynamoRIO-MacOS-<ver>.tar.gz` name this step assumed is hypothetical. The
> project README confirms the status — *"Mac OSX support is in progress"* — and the
> [releases page](https://dynamorio.org/page_releases.html) advertises only the
> Windows + Linux variants. **Conclusion: Step 0 fails its own gate → STOP.** A
> prebuilt-release M0 is impossible. The only path forward is building DynamoRIO
> from source for macOS (separate, larger effort; not scoped by this plan), and
> even then the Application Interface is the least-maintained DR macOS path — so it
> is a research spike, not plumbing. M1/M2 remain held.

**Step 1 — A compiled-function smoke test (`examples/test_drtrace_macos.c`).**

Do **not** start from the existing `test_drtrace.c` (it depends on the
generated-bytes path). Write a minimal harness that traces a function the C
compiler emitted into the binary:

```c
/* compiled into __TEXT — no exec_alloc, no W^X */
__attribute__((noinline)) static long add2(long a, long b) { return a + b; }

/* ... dr_init / dr_start ... */
asmtest_trace_t *tr = asmtest_trace_new(64, 64);
asmtest_dr_register_region("add2", (void *)add2, 64, tr);
asmtest_trace_begin("add2");
long r = add2(20, 22);
asmtest_trace_end("add2");
/* assert r == 42 && asmtest_trace_covered(tr, 0) */
```

Symbol mode (`asmtest_dr_register_symbol("asmtest_symbol_demo", …)`) is an
equally valid M0 target and exercises the same `dr_get_proc_address` path. Use
whichever resolves more cleanly on Mach-O; both avoid executable memory.

**Step 2 — Fix `dr_lib_path()` for the `.dylib` extension.**

The path-building `snprintf`s in
[drtrace_app.c:51-71](../../../src/drtrace_app.c#L51-L71) and the duplicate in
`asmtest_dr_available()` ([drtrace_app.c:124-138](../../../src/drtrace_app.c#L124-L138))
hardcode `.so`:

```c
#if defined(__APPLE__)
#  define DR_LIBNAME "libdynamorio.dylib"
#else
#  define DR_LIBNAME "libdynamorio.so"
#endif
```

Pin the `lib64/release/` subdir to the Step-0-verified path; add a Darwin variant
if it differs.

**Step 3 — Makefile: platform-conditional DR paths and client name.**

```makefile
ifeq ($(UNAME_S),Darwin)
DR_LIBDIR := $(DYNAMORIO_HOME)/lib64/release   # update per Step 0
DR_DLLIB  := $(DR_LIBDIR)/libdynamorio.dylib
DR_CLIENT := $(BUILD)/libasmtest_drclient.dylib
else
DR_LIBDIR := $(DYNAMORIO_HOME)/lib64/release
DR_DLLIB  := $(DR_LIBDIR)/libdynamorio.so
DR_CLIENT := $(BUILD)/libasmtest_drclient.so
endif
```

Replace the hardcoded `.so` in: the `DR_AVAILABLE` `wildcard $(DR_DLLIB)` probe;
the `drtrace-client` target + `libasmtest_drclient.so` prerequisite; the
`ASMTEST_DRCLIENT=` env var in `drtrace-test`; and the skip message
(`DynamoRIO-Linux-<ver>` → `DynamoRIO-$(UNAME_S)-<ver>`). Leave the
`docker-drtrace*` lanes as `.so` — they run Linux containers regardless of host;
add a clarifying comment.

Guard the link flags (cosmetic, but do it while here):

```makefile
ifeq ($(UNAME_S),Darwin)
DRTRACE_LDFLAGS := -rdynamic -lpthread      # -rdynamic -> -export_dynamic on clang
else
DRTRACE_LDFLAGS := -rdynamic -ldl -lpthread
endif
```

**Step 4 — Build and run.**

```
DYNAMORIO_HOME=/path/to/DynamoRIO-MacOS-<ver> make drtrace-client
DYNAMORIO_HOME=/path/to/DynamoRIO-MacOS-<ver> make drtrace-test-macos   # new target for the M0 harness
```

Watch, in order of likelihood-to-fail:
1. `dlopen(libdynamorio.dylib)` and `dr_app_setup()` succeed.
2. `dr_app_start()` returns without crashing (the Mach takeover risk).
3. The client resolves the markers via `dr_get_proc_address` (the Mach-O symbol
   risk) — if markers don't resolve, `begin/end` are no-ops and no coverage is
   recorded even though nothing crashes.
4. The client's block callback writes through the app-owned `asmtest_trace_t`.
5. `dr_register_signal_event` / `DR_SIGNAL_DELIVER` path doesn't crash.
6. `dr_app_stop_and_cleanup()` returns the process to native cleanly.

**Acceptance.** On macOS x86-64 with a macOS DR release, the new compiled-function
harness records block offset `0` for a normally-compiled function and shuts down
cleanly.

**Effort.** 2–4 days *if DR macOS attach works* — dominated by the Step-0 unknown,
not the code. Could be an immediate no-go.

**Go/no-go.** If attach crashes, or markers don't resolve, file the root cause and
**hold M1/M2** until DR's macOS support is good enough. The Phase 1–5 Linux work
is unaffected either way.

> **Resolved 2026-06-30: NO-GO.** The spike never reached attach — it stops at
> Step 0, because no macOS DynamoRIO release exists to attach *with* (see the
> Step-0 result above). Root cause filed: **blocked upstream**, not a defect in
> this repo. M1/M2 held. Revisit only if DynamoRIO begins publishing a macOS
> release that ships the Application Interface + client CMake support, **or** a
> separately-scoped from-source macOS DR build is undertaken.

---

## Phase M1 — Generated-code (W^X) path, x86-64 then arm64 *(planned)*

**Goal.** Make the generated-bytes path (`asmtest_exec_alloc`,
`asmtest_asm_exec_native`) work on macOS, and make the existing
`test_drtrace.c`-style generated-code tests runnable. This is where W^X,
`MAP_JIT`, and the entitlement model live.

**Prerequisite.** M0 green on x86-64.

### M1a — x86-64 generated code

On a real Intel Mac, the existing `PROT_NONE → mprotect(RW) → memcpy →
mprotect(RX)` path in
[drtrace_app.c:423-444](../../../src/drtrace_app.c#L423-L444) is expected to work
(Intel does not enforce hardware W^X). **Under Rosetta 2 this is not a safe
assumption** — Rosetta translates the x86-64 process and has its own JIT
write-protect semantics; the `mprotect`-based JIT pattern is exactly the uncertain
case. Treat "x86-64 generated code under Rosetta" as **must-verify**, not as
known-good. The dev machine (Intel x86-64) covers the native-Intel case directly.

**Acceptance (M1a).** The existing generated-bytes assertions in a `test_drtrace`
run pass on native Intel macOS; the Rosetta result is recorded (pass or
documented-unsupported).

> **RESULT 2026-07-22 — native Intel: PASS.** `drtrace-test-macos` (extended by
> [macos-dynamorio-port.md](../implementations/macos-dynamorio-port.md) T6 to
> also build and run `test_drtrace` after the M0 harness, `DRAPP_KEYSTONE=0`)
> is green on the macOS 14.7.5 / Intel Core i7 dev host against the fork-built
> `libdynamorio.dylib` (`make dynamorio-macos` home): **18/18 `ok`, three
> consecutive runs, 0 failures** — `exec_alloc` W^X (`PROT_NONE → RW → RX`)
> maps and executes, coverage accumulates across re-runs, instruction mode
> records the exact ordered offset sequence `[0x0,0x3,0x6,0xc,0x11]`, the
> cap-1 truncation bit sets, and symbol mode resolves — on top of the 13/13 M0
> harness in the same invocation.
> **Rosetta: not run — no Apple Silicon hardware on this host.** The Rosetta
> leg (`make CC="cc -arch x86_64" drtrace-test-macos` on an arm64 Mac) stays
> must-verify and is recorded as gated, per T6's honesty rule.

### M1b — arm64 generated code

**Two distinct problems, ranked by severity.**

**Problem 1 (architectural, not just code): the entitlement attaches to the main
executable, not the dylib.** `MAP_JIT` on Apple Silicon requires
`com.apple.security.cs.allow-jit` **on the signature of the process's main
executable** when running under the hardened runtime. Signing
`libasmtest_drapp.dylib` does **nothing** — a dylib's entitlements don't grant the
loading process anything. Consequence:

- **Standalone executable** (`test_drtrace`): signable with the entitlement →
  generated-code path can work.
- **Language bindings** (Python/Node/Ruby dlopening `libasmtest_drapp.dylib`):
  the **interpreter** (`python3`, `node`, `ruby`) would need the entitlement.
  Homebrew interpreters are typically ad-hoc signed (no hardened runtime → may
  not need it); system/notarized interpreters are hardened and **cannot be
  re-signed** without breaking notarization. So the generated-code path is
  **effectively unavailable from the standard bindings on hardened arm64
  interpreters.** The bindings' viable arm64 path is the **compiled-function /
  symbol** mode (no `MAP_JIT`), per the distinction table above. State this as a
  documented limitation, not a TODO.

Whether the entitlement is needed at all depends on hardened-runtime status: an
ad-hoc-signed (`codesign -s -`, no `--options runtime`) process can often use
`MAP_JIT` without it. Verify the actual requirement on the target interpreters
rather than assuming.

**Problem 2 (mechanical): the W^X dance itself.** Isolate the platform exec-memory
code behind helpers so the Linux/Intel path is untouched:

```c
#if defined(__APPLE__) && defined(__aarch64__)
#include <pthread.h>   /* pthread_jit_write_protect_np */
static int exec_alloc_platform(size_t len, void **outp) {
    void *p = mmap(NULL, len, PROT_READ|PROT_WRITE|PROT_EXEC,
                   MAP_PRIVATE|MAP_ANONYMOUS|MAP_JIT, -1, 0);
    if (p == MAP_FAILED) return -1;
    *outp = p; return 0;
}
static void exec_copy(void *dst, const uint8_t *src, size_t len) {
    pthread_jit_write_protect_np(0);   /* open write window */
    memcpy(dst, src, len);
    pthread_jit_write_protect_np(1);   /* close: executable */
    __builtin___clear_cache((char *)dst, (char *)dst + len);
}
#else  /* Linux + macOS x86-64: PROT_NONE -> RW -> RX */
static int exec_alloc_platform(size_t len, void **outp) {
    void *p = mmap(NULL, len, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return -1;
    if (mprotect(p, len, PROT_READ|PROT_WRITE) != 0) { munmap(p, len); return -1; }
    *outp = p; return 0;
}
static void exec_copy(void *dst, const uint8_t *src, size_t len) {
    memcpy(dst, src, len);
    (void)mprotect(dst, len, PROT_READ|PROT_EXEC);  /* caller checks via out=NULL */
    __builtin___clear_cache((char *)dst, (char *)dst + len);
}
#endif
```

Refactor both `asmtest_exec_alloc()` and `asmtest_asm_exec_native()` onto these
so the existing paths are byte-for-byte unchanged off arm64-macOS.

Entitlement for the **standalone test executable only** (`drtrace.entitlements`,
new file: `com.apple.security.cs.allow-jit = true`), applied via a Darwin Makefile
step: `codesign --entitlements drtrace.entitlements -s - $(BUILD)/test_drtrace`.
Do **not** waste a step signing the dylib (Problem 1).

**Problem 3 (test fixture): the smoke test has x86-64 bytes.**
`ROUTINE[]` in [test_drtrace.c:47-54](../../../examples/test_drtrace.c#L47-L54) is
x86-64 machine code and cannot run on arm64. arm64 acceptance therefore needs one
of: (a) an arm64-encoded `ROUTINE[]` variant under `#if defined(__aarch64__)`, or
(b) routing arm64 acceptance through the compiled-function/symbol harness from M0
(no generated bytes). Option (b) is cheaper and is the recommended arm64
acceptance.

**arm64 Keystone note.** `asmtest_asm_exec_native()` hardcodes `ASM_X86_64`
([drtrace_app.c:458](../../../src/drtrace_app.c#L458)); on arm64 it should return
`ASMTEST_DR_ENOSYS` explicitly rather than assemble x86-64 for an arm64 host.
arm64 Keystone host-native assembly is out of scope here.

**Prerequisite.** M0 green **and** a confirmed, stable arm64 DR macOS binary
(fat or separate tarball, per Step 0). If DR arm64/macOS isn't ready, M1b waits;
M1a and the bindings' compiled-function path still deliver value.

**Acceptance (M1b).** On Apple Silicon: the compiled-function harness passes
(no entitlement needed); the standalone generated-code test passes after
codesigning `test_drtrace`; the binding-level limitation (no generated code on
hardened interpreters) is documented.

> **RESULT 2026-07-22 — implementable slice landed
> ([macos-dynamorio-port.md](../implementations/macos-dynamorio-port.md)
> T7+T8); arm64 acceptance NOT RUN (two gates).**
> **What ran (Intel-mac dev host):** the Problem-2 helpers
> (`exec_alloc_platform`/`exec_copy`) landed with the arm64-macOS arm exactly
> as sketched (`MAP_JIT` RWX map + per-thread `pthread_jit_write_protect_np`
> toggle; helpers return int so the existing `mprotect(RX)` failure check
> survives, and copy/seal lengths are carried separately — pure refactor
> elsewhere, `make docker-drtrace` green); `asmtest_asm_exec_native` returns
> `ASMTEST_DR_ENOSYS` off x86-64; `drtrace.entitlements` + the Darwin
> `codesign -f --entitlements … -s -` step on `$(BUILD)/test_drtrace` landed
> (`-f` because arm64 ld ad-hoc-signs at link time — a plain `-s -` would
> refuse on exactly the arm64 leg; no-op on Intel), and
> `codesign -d --entitlements :-` prints the `allow-jit` key with the signed
> harness still 18/18 green.
> **Extent of MAP_JIT exercise: compile + link only** — `cc -arch arm64`
> cross-compile exits 0 and `nm` shows `_pthread_jit_write_protect_np`
> referenced in the arm64 object (absent from x86-64), but no MAP_JIT
> allocation has ever executed.
> **Gated (both required before arm64 acceptance can run):** Apple Silicon
> hardware — this host is Intel; an arm64/universal macOS DR runtime —
> upstream i#5383 still open. Acceptance routes through the M0
> compiled-function harness (option (b)); no arm64 `ROUTINE[]` fixture is
> planned.

**Effort.** 2–4 days on confirmed arm64 hardware, after M0.

---

## Phase M2 — Bindings, Makefile cleanup, CI *(planned)*

**Goal.** Wire the host-side per-language drtrace lanes, finish Makefile cleanup,
and add a macOS CI job — scoped to what the previous phases proved actually works.

**Bindings.** Expose **compiled-function / symbol mode** as the supported macOS
binding path (works on both arches, no entitlement). Document the generated-code
path as standalone-executable-only on hardened arm64. Point host-side lanes at
`$(DR_CLIENT)` rather than the bare `.so`.

**Makefile cleanup.** Apply the `-ldl`-on-Darwin removal; finish the skip-message
arch fix; keep `docker-drtrace*` on Linux `.so` paths with a clarifying comment.

**CI.** Add a `drtrace-macos` job to `.github/workflows/ci.yml`, separate from the
main `test` job so a DR failure never blocks the Unicorn tier:

```yaml
drtrace-macos:
  runs-on: macos-15-intel      # Intel: M0/M1a, no MAP_JIT entitlement question
  steps:
    - uses: actions/checkout@v4
    - name: Download + pin DynamoRIO macOS release
      run: |
        curl -L <pinned-dr-macos-url> -o dr.tar.gz && tar -xzf dr.tar.gz
        echo "DYNAMORIO_HOME=$(pwd)/DynamoRIO-MacOS-<ver>" >> $GITHUB_ENV
    - run: make drtrace-client drtrace-test-macos
```

- Start on **`macos-15-intel` (Intel)** (`macos-13` was retired 2025-12-08 —
  [_positions.md #6](../implementations/_positions.md)) — it exercises M0 + M1a and dodges the arm64
  entitlement question entirely. Add `macos-latest` (arm64) only after M1b is
  proven, and expect the hosted-runner hardened-runtime/entitlement constraint to
  bite (ad-hoc signing may not grant `allow-jit` there); a self-hosted arm64
  runner with a real signing identity may be required.
- Pin the DR version the same way the Linux job does.

**Effort.** 1–2 days.

---

## Risks and open points

- **DR macOS attach may simply not work** — the dominant risk, stated at the top.
  M0 is the go/no-go; M1/M2 are contingent.
- **Mach-O marker resolution** (`dr_get_proc_address`) is load-bearing for the
  whole region mechanism and is unverified on macOS — part of M0, not a footnote.
- **arm64 entitlement model** makes generated code unavailable from hardened
  interpreters; compiled-function/symbol mode is the binding path on arm64.
- **Rosetta x86-64 W^X** is must-verify, not known-good.
- **DR arm64/macOS release availability** gates M1b; M0/M1a are independent.
- **Signal vs. Mach exceptions.** `dr_register_signal_event`/`DR_SIGNAL_DELIVER`
  ([drtrace_client.c:381-398](../../../src/drtrace_client.c#L381-L398)) abstracts
  Mach exceptions as POSIX signals; the client should work unchanged but the
  SIGSEGV-chaining behaviour must be confirmed in M0.
- **The raw core API** (`dr_register_bb_event`, clean calls, `dr_get_proc_address`)
  chosen for Linux robustness is still the right choice on macOS and avoids
  re-opening the drmgr/drwrap extension question (the glibc `__memcpy_chk` failure
  that drove that choice doesn't apply on macOS, but nothing is gained by
  reintroducing the extensions).

## Phasing summary

```
M0 (x86-64 attach + compiled-fn)  ── go/no-go for everything ──┐
   │                                                            │
   ├─ M1a (x86-64 generated code; Rosetta = verify)             │
   │                                                            │
   └─ M1b (arm64: W^X + MAP_JIT + entitlement)  ── needs arm64 DR + arm64 HW
                                                                │
M2 (bindings = compiled-fn path; Makefile; CI on macos-15-intel) ┘
```

Run M0 first. Do not invest in M1b until M0 passes and a stable arm64 DR macOS
release is confirmed. The Linux Phase 1–7 tier is unaffected throughout.
