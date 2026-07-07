# asm-test — repository review (2026-07-01)

**Scope:** whole-repo review for correctness bugs and areas for improvement.
**Method:** five parallel focused passes (core C runtime & capture, native-trace
tiers, build/packaging/CI, language bindings & emulator, documentation accuracy),
with the load-bearing findings re-verified against the source by hand.

Paths below are repo-relative; `file:line` points at the exact site. Items
marked **[verified]** were confirmed against the code during the review.

---

## Overall

This is a mature, unusually complete codebase — ~33k LOC of C, 10 language
bindings, a Unicorn emulator tier, four native-trace backends, a broad CI matrix,
guard-page allocation, fork isolation, and no stray `TODO`/`FIXME` markers. The
findings here sharpen a strong project rather than rescue a broken one. The
highest-value class is not crashes but **"green CI, wrong artifact"** gaps and a
**README that oversells** what the rest of the docs honestly describe as
self-skipping.

### Severity summary

| # | Finding | Severity | Area |
|---|---------|----------|------|
| 1 | Leaked traced child on the fault path | High | native-trace |
| 2 | RNG division-by-zero on wide ranges | High | core |
| 3 | In-process crash containment gaps (`--no-fork`/`--bench`) | Medium–High | core |
| 4 | Signed-overflow UB in ULP helpers (trips own UBSan lane) | Medium | core |
| 5 | Truncated-record OOB read + PT barrier/degrade gaps | Low–Medium | native-trace |
| 6 | Version bump doesn't touch the C header | High | packaging |
| 7 | `DRAPP_KEYSTONE` flag not in prerequisites | Medium–High | build |
| 8 | Shell robustness in clean-room trust anchor | Medium | packaging |
| 9 | Lua binding drops integer args | Medium | bindings |
| 10 | Emulator doesn't reset state between calls | Medium | emulator |
| 11 | Node 64-bit precision loss; weak parity tripwire | Low | bindings |
| 12 | README "everything ships" oversells | Medium | docs |
| 13 | AVX-512 doc stale vs shipped code | Low–Medium | docs |
| 14 | DESIGN.md stale (non-compiling example) | Low–Medium | docs |
| 15 | Over-documentation (plans/analysis sprawl) | Low | docs |

---

## Correctness bugs

### 1. Leaked traced child on the fault path — High **[verified]**
`src/ptrace_backend.c:696-702`

When a traced routine takes a real signal (SIGSEGV/SIGILL — i.e. exactly the
buggy routines this tier exists to trace), the single-step loop breaks with
`rc == ASMTEST_PTRACE_OK`, so control reaches `normalize()` at `src/ptrace_backend.c:750`
and returns **without** the `kill(pid, SIGKILL)` / `waitpid()` that every other
exit path performs (`PTRACE_O_EXITKILL` only fires when the *tracer* exits, not
when this function returns). Each faulting-routine trace leaks a stopped,
unreaped child; a suite of them exhausts PIDs. This is the finding most likely to
bite in normal use of the ptrace backend.

**Fix:** on the non-SIGTRAP break, `kill(pid, SIGKILL); waitpid(pid, &status, 0);`
as the `EXIT_CALLOUT_LOST` and `rc != OK` paths already do.

### 2. RNG division-by-zero on wide ranges — High **[verified]**
`src/asmtest.c:585-590`

```c
long asmtest_rng_range(asmtest_rng_t *rng, long lo, long hi) {
    if (hi <= lo) return lo;
    uint64_t span = (uint64_t)(hi - lo) + 1;   /* hi-lo overflows; +1 wraps to 0 */
    return lo + (long)(asmtest_rng_u64(rng) % span);
}
```

A generator drawing over a range wider than `LONG_MAX` — e.g.
`asmtest_rng_range(rng, LONG_MIN, LONG_MAX)`, a natural thing in differential /
property testing — makes `hi - lo` signed-overflow UB, and `(uint64_t)(hi-lo)+1`
evaluates to `0`, so `% span` is **integer division by zero → SIGFPE** and the
runner crashes (or, under `--fork`, reports a spurious "crashed by signal 8").

**Fix:** compute `span` entirely in unsigned and special-case the full range
(`span == 0` means "all values").

### 3. In-process crash containment gaps — Medium–High **[verified]**
`src/asmtest.c:708-739` (handlers), `763-795` (`run_one`)

The default fork model contains these; the in-process paths (`--no-fork`, always
for `--bench`, and any fork/pipe-failure fallback) do not:

- **No alternate signal stack.** `install_handlers` sets `sa_flags = 0` with no
  `SA_ONSTACK`/`sigaltstack`, so a **stack-overflow** SIGSEGV in a routine under
  test cannot run the handler and kills the whole runner. For a framework whose
  purpose is containing crashes, the in-process path should install
  `sigaltstack` + `SA_ONSTACK`.
- **Async-signal-unsafe timeout handler.** On SIGALRM, `asmtest_on_signal` calls
  `snprintf` and then `siglongjmp`s back into buffered `printf`/`malloc`. When the
  alarm interrupts the body in-process this can deadlock/corrupt on glibc locks
  held at interrupt time. (Synchronous crash signals are less of a concern.)
- **Stale `jmp_buf` race** (`src/asmtest.c:778-793`): a late SIGALRM can fire after
  the teardown `sigsetjmp` frame has returned but before `alarm(0)`, longjmp-ing
  into a returned frame (UB). Move `alarm(0)` to immediately after `tc->fn()`.

### 4. Signed-overflow UB in the ULP helpers — Medium **[verified]**
`src/asmtest.c:302-322` (`fp_ulp_distance`, `fp_ulp_distance_f`)

`ia - ib` overflows `int64_t` for far-apart operands (e.g.
`ASSERT_DNEAR(-DBL_MAX, DBL_MAX, ...)`), and the `INT64_MIN - ia` sign-flip
overflows for any negative operand. It yields the intended magnitude on
two's-complement hardware but is UB — and **traps under this repo's own
`make sanitize` (UBSan) lane**, so it is self-inconsistent.

**Fix:** perform the subtraction and sign-flip in `uint64_t`/`uint32_t`.

### 5. Native-trace robustness (lower severity)
- `src/hwtrace.c:525` — reads 8 bytes at `body` (the sample `nr`) before bounds-
  checking against a truncated `8 ≤ h->size < 16` record. The kernel never emits
  these, but the code otherwise treats the ring as untrusted, so this is an
  inconsistency; require `off + sizeof(*h) + sizeof(uint64_t) <= span` first.
- `src/hwtrace.c:735-748` — the Intel PT drain reads `aux_head` with no `smp_rmb`
  before consuming the AUX ring, unlike the data-ring paths that do
  `__sync_synchronize()`. Mostly moot because capture is disabled first, but
  inconsistent.
- `src/hwtrace.c:617-625,713-730` — a failed AMD/PT/single-step `begin` leaves the
  caller's `asmtest_trace_t` unmodified (no `truncated` flag), so a silent capture
  failure looks like a passing empty trace. Consider `trace->truncated = true` on
  begin failure.
- `src/ptrace_backend.c:196,227-228` — jitdump `code_size` (untrusted `uint64_t`)
  is used in signed subtractions without validating `code_size <= total - 56`.
  Bounded downstream, so hardening rather than an exploit.

---

## Build / packaging — the "green CI, wrong artifact" class

### 6. A version bump doesn't touch the C header — High **[verified]**
`include/asmtest.h:37-40`, `scripts/sync-version.sh`, `scripts/amalgamate.sh:16`

`include/asmtest.h` hardcodes the version in four macros
(`ASMTEST_VERSION_MAJOR/MINOR/PATCH` + the `ASMTEST_VERSION` string), and
`amalgamate.sh` derives the single-header version *from that header*. But
`sync-version.sh` / `make check-version` only cover the nine binding manifests.
So `make sync-version && make check-version` pass green while `ASMTEST_VERSION`,
`ASMTEST_VERSION_NUM`, and the amalgamated `asmtest_single.h` all stay at the old
version. The header is an unchecked second source of truth.

**Fix:** extend both the sync (write) and the check to `include/asmtest.h`.

### 7. `DRAPP_KEYSTONE` flag not captured in object prerequisites — Medium–High **[verified]**
`mk/native-trace.mk:49-51` and `77-79`

`drtrace_app.o` is compiled with `$(DRAPP_KS_DEF)` (`-DASMTEST_HAVE_KEYSTONE`,
gated by the `DRAPP_KEYSTONE` knob at `mk/native-trace.mk:39`) but that flag is
not among the rule's prerequisites. Flipping the knob between invocations in the
same `build/` tree reuses the stale object — and the file's own comment
(`:46-48`) notes a Keystone-enabled drapp has unresolved `emu_*` symbols and
won't `dlopen`. The same root cause makes `make -j` on the aggregate
`drtrace-bindings-test` targets fragile (concurrent sub-makes racing the same
CMake dir / `.o`/`.so`), masked today only because CI runs each Docker lane in a
clean tree.

**Fix:** fold the active flag knobs (`DRAPP_KEYSTONE`, and by extension `SAN`,
`COV`, `ASM_SYNTAX`) into the object-file identity, e.g. via a `.flags` sentinel
prerequisite.

### 8. Shell robustness in the clean-room trust anchor — Medium
- `scripts/assert-clean-path.sh` has no `set -u`; with an unset `TMPDIR`, the
  temp-path escape hatch (`:69`) becomes the glob `*`, which matches **any** path
  and would accept a leaked library as "clean" — weakening the exact assertion
  the "proven, not trusted" packaging story rests on. Add `set -u` and guard
  `${TMPDIR:-/nonexistent}`.
- `scripts/clean-room-test.sh:204` — `printf "$summary"` treats path-derived data
  as a format string (a `%` in a resolved path garbles output). Use
  `printf '%b' "$summary"`.
- `scripts/verify-macho.sh:76-84` — the install-name leak assertion (`otool -D`
  + `grep '^(@|/)'` + `head -1`) can't reliably distinguish "no id" from an
  absolute (leaked) id; the belt-and-suspenders `otool -L` scan still catches the
  leak, but the 2a check can emit a misleading `ok`.

---

## Bindings + emulator

### 9. Lua binding silently drops integer args — Medium **[verified]**
`bindings/lua/asmtest.lua:252,266`

`Emu:call_fp` and `Emu:call_vec` hardcode `nil, 0` for the C
`iargs`/`niargs` parameters, so a Lua caller cannot pass integer arguments to a
mixed routine like `f(long n, double x)` — `rdi` is left as garbage and the call
returns a wrong result with no error. Every peer binding exposes them (Python
`core.py:329,347`, Node `asmtest.js:261,269`, Ruby `asmtest.rb:357,367`, Go
`asmtest.go:478,485`), and the C side fully supports it
(`src/emu.c:410-425`). The Lua conformance test only exercises FP/vector-only
args, so it never catches this.

### 10. Emulator doesn't reset state between calls on a reused handle — Medium
`src/emu.c:246-439`

`emu_x86_setup_sysv` sets only rsp + the N integer-arg registers;
`emu_call_fp`/`emu_call_vec` write only the passed FP/vector regs. Other GP
registers, the rest of the vector file, and guest RW memory are never cleared.
All bindings hold one long-lived handle across many `call()`s (the intended
usage, and what `emu_fuzz_cover1`/`emu_mutation_test1` do), so a routine that
reads a register/lane the caller didn't set sees the previous run's state → the
same call can return different results depending on history. Mostly latent
(pure-GP-arg routines are unaffected), but a real nondeterminism footgun. Either
document "registers you don't pass are undefined" or add a reset.

### 11. Lower-severity binding items
- `bindings/node/asmtest.js:231-235,359-368` — 64-bit register/address values are
  funneled through `Number()`, losing precision above 2^53, so
  `assertEmuReg("rax", 0xFFFFFFFFFFFFFFFF)` can mis-compare. Go (native uint64)
  and Python (bignum) don't have this.
- `scripts/check-bindings-parity.sh:52-78` — counts a symbol as "wrapped" if
  `git grep -wF` finds the *name* anywhere in the binding (incl. cdef strings and
  comments), and doesn't cover the emulator tier at all — so it would report
  finding #9 as "in sync." Useful for the totally-missing case; the docs'
  "uniform parity" claim overstates what it proves.

---

## Documentation

### 12. README oversells — Medium **[verified]**
`README.md:9` ("Every planned capability ships today") contradicts the rest of
the docs. On any host the following do not run live:

- **CoreSight is a scaffold that never decodes on any host.**
  `asmtest_cs_decoder_present()` returns `0` in **both** build arms
  (`src/cs_backend.c:108` with OpenCSD, `:122` without), so the tier self-skips
  even on a real AArch64 CoreSight board.
- **Intel PT / AMD LBR** compile but self-skip off their specific bare-metal
  hardware; **AArch64 ptrace single-step** self-skips under qemu-user; the
  **eBPF code-image detector** self-skips without libbpf/CAP_BPF/BTF.

`docs/hardware-tracing.md`, `docs/native-tracing.md`, and `docs/features.md`
already describe these honestly. **Fix:** split "ships and runs on any host"
(emulator, single-step, DynamoRIO on Linux-x86-64) from "compiles, self-skips off
its hardware."

### 13. AVX-512 doc stale vs shipped code — Low–Medium **[verified]**
`docs/floating-point-simd.md:124-126` says native 512-bit capture is "not yet
wired," but `asm_call_capture_vec512` (`src/capture.asm:305`,
`include/asmtest.h:316`) and the `ASM_VCALL512_*` / `ASSERT_VEC512_EQ` macros are
fully shipped. `docs/api-reference.md` documents none of the `vec512` API.
(AArch64 SVE genuinely is still absent — only the AVX-512 half of that sentence is
wrong.)

### 14. DESIGN.md is a stale design-era artifact — Low–Medium **[verified]**
`DESIGN.md:48,93,137-139` call `asm_call2_capture` / `asm_call1_capture` — no such
symbols exist (the API is the `ASM_CALLn` macro family), so the sample won't
compile. It also still describes the surface as "narrow: ≤6 integer args"
(`:198-200`, ~9 phases out of date), targets "x86-64 macOS ... AArch64 is a later
phase" (`:9-10`), and lists a 3-file `src/` (`:59-68`; actual: 25). The
user-facing `docs/` tree is accurate; DESIGN.md is not.

### 15. Over-documentation — Low
16.6k lines of Markdown across 77 files vs ~12k lines of `src/`+`include/`,
concentrated in **15 plan docs + 4 analysis docs (~7.7k lines)**. The trace
subsystem alone spans 15 documents, with AMD tracing split across four
overlapping files, and `docs/analysis/trace-parity-matrix.md` restates tables
that also live in `docs/features.md` and the plans. Much of `docs/plans/` and
`docs/analysis/` reads like design scratchpad kept in-tree rather than pruned
after landing.

---

## What's solid (checked and cleared)

Guard-page allocation math; the fork/pipe wire protocol (`< PIPE_BUF`,
EINTR-safe, no deadlock); capture-trampoline register offsets vs the header
static-asserts; struct-mirror offsets across Rust/Zig/C++/Java/.NET (every pinned
offset exact); emulator leak/fault handling and x86-only guard scoping; the
`sync-version` regexes against all nine manifests; and the auto-select cascade
order (`src/trace_auto.c`) matching the docs. Several plausible-but-wrong leads
were investigated and cleared rather than reported (JUnit suite grouping, glob
OOB, vector buffer sizing, `cs_reconstruct` bounds).

---

## Suggested fix order

Highest leverage first:

1. **#1** — ptrace child leak (real, common in normal use of that tier).
2. **#6, #7** — version + Keystone-flag staleness (silent wrong artifacts).
3. **#2, #4** — RNG crash + UBSan-tripping UB (small, self-contained, verifiable).
4. **#9** — Lua arg-parity; **#12** — README honesty.
5. **#3, #8, #10** — conditional/hardening; **#15** — curation.
