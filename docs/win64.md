# Native Win64 tier

asm-test can capture and assert a routine running under the **Microsoft x64
("Win64") ABI on real x86-64 silicon**, alongside the
[emulator](emulator.md)'s `emu_call_win64`, which runs Win64 bytes on a System V
host. The native tier exercises the real ABI on real hardware — and, crucially,
**without a Windows host**: it cross-compiles to a Windows PE and runs it under
Wine, or drives the trampoline directly via a compiler ABI attribute.

> Scope: the **capture** tier — `call` a routine through the real Win64 ABI and
> snapshot its registers/flags/ABI-preservation — ships today and runs
> `--no-fork`. The framework's POSIX runner guarantees (per-test fork isolation,
> timeouts, guard pages, the `-jN` pool, in-process crash-to-failure) rest on
> POSIX primitives with no Win32 equivalent; porting them is the **runner port**
> (Phase 4), now underway — all five primitives are ported and verified under
> Wine, with the runner's execution model being wired through a platform seam
> (see [The runner port](#the-runner-port-phase-4) below). See the
> [implementation plan](https://github.com/wilvk/asm-test/blob/main/docs/plans/win64-native-tier-plan.md)
> for the full breakdown.

## What it captures

The trampoline (`src/capture_win64.asm`) mirrors all eight System V
`asm_call_capture*` variants for the Microsoft x64 convention:

| Entry point | Captures |
|---|---|
| `asm_call_capture_win64` | return + GP callee-saved + RFLAGS, 6 int args |
| `asm_call_capture_args_win64` | arbitrary integer arity (register + stack) |
| `asm_call_capture_fp_win64` | + FP args (xmm0–3) and the FP return |
| `asm_call_capture_fp_n_win64` | arbitrary FP arity |
| `asm_call_capture_vec_win64` | full vector file (xmm0–15) + `xmm6–15` preservation |
| `asm_call_capture_vec_n_win64` | arbitrary vector arity |
| `asm_call_capture_sret_win64` | struct return via the hidden pointer |
| `asm_call_capture_bigstruct_win64` | large struct args (by reference) |

The captured state lands in the Win64 layout of `regs_t`, selected by
`-DASMTEST_ABI_WIN64` (see [ABI capture](abi-capture.md)). The
**ABI-preservation** check covers the Win64 callee-saved set, which is *larger*
than System V's: it adds `rdi`/`rsi` to the integer side and treats **`xmm6–15`
as callee-saved** on the vector side. `ASSERT_ABI_PRESERVED(&r)` verifies the
integer set; `ASSERT_ABI_PRESERVED_VEC(&r)` verifies the `xmm6–15` set after a
`_vec`/`_vec_n` capture (both macros are compiled in only under
`-DASMTEST_ABI_WIN64`).

### Calling and asserting from C

The `asm_call_capture_*_win64` entry points are the binding-ABI surface; in a C
suite use the `ASM_CALL_WIN64_*` convenience macros, the Win64 counterparts of
[`ASM_CALL0`…`ASM_CALLN`](abi-capture.md). They marshal `long long` arguments
(Win64 is LLP64, so an integer slot is 64-bit) and are also gated on
`-DASMTEST_ABI_WIN64`:

| Macro | Calls with |
|---|---|
| `ASM_CALL_WIN64_0`…`ASM_CALL_WIN64_6(&r, fn, …)` | 0–6 integer register args |
| `ASM_CALL_WIN64_N(&r, fn, …)` | Any number of integer args (overflow on the stack) |

```c
regs_t r;
ASM_CALL_WIN64_2(&r, add_signed, 2, 3);
ASSERT_EQ(r.ret, 5);
ASSERT_ABI_PRESERVED(&r);        // rbx, rbp, rdi, rsi, r12–r15 restored
```

For the FP, vector, struct-return, and big-struct paths, call the matching
`asm_call_capture_fp_win64` / `_vec_win64` / `_sret_win64` / `_bigstruct_win64`
entry points directly (see the table above).

### Win64 vs. System V

The deltas the trampoline models (all also encoded in the emulator):

- integer args in `rcx, rdx, r8, r9` (not `rdi, rsi, …`), then the stack;
- a **32-byte shadow space** reserved below the return address at every call site;
- `rdi`/`rsi` are callee-saved (argument registers on System V);
- **`xmm6–xmm15` are callee-saved** (all xmm are volatile on System V);
- large structs are passed **by reference**, not inline on the stack.

## Running it — no Windows host

Two lanes, same trampoline and suite:

```sh
make win64-msabi-test   # native lane: __attribute__((ms_abi)) on an x86-64 host
make docker-win64       # PE lane: nasm -f win64 + mingw-w64, run under Wine
```

- **Native (`ms_abi`) lane.** On an x86-64 Linux/macOS host the CPU is already
  x86-64; only the ABI differs. The trampoline is assembled for the host object
  format and called through GCC/Clang's `__attribute__((ms_abi))`, so the Win64
  convention is exercised natively with no Wine and no PE. Fastest feedback;
  x86-64 only.
- **PE + Wine lane.** Cross-assemble with `nasm -f win64`, link with
  `x86_64-w64-mingw32-gcc` into a real Windows PE, and run it under `wine64` in an
  isolated Docker image (`Dockerfile.win64`, on the shared bindings base). This is
  the closest non-Windows approximation of a Windows host: a real PE loader and
  Win32 personality.

Both replay the same capture suite (`tests/win64/test_capture_win64.c`), which
doubles as the **native Win64 conformance check**. The CI `win64` job runs both on
every push.

### Layout manifest

```sh
make manifest-win64     # -> asmtest_abi_win64.json
```

emits the machine-readable Win64 `regs_t` layout (`"abi": "win64"`), the analog of
the System V [manifest](integration.md) bindings mirror.

## The runner port (Phase 4)

The capture tier above runs as a plain `--no-fork` program. The framework's
*runner* guarantees — per-test crash isolation, timeouts, guard pages, the `-jN`
parallel pool, and in-process crash-to-failure — lean on POSIX primitives that a
Win32 personality doesn't provide. Phase 4 ports each to its Win32 equivalent in
`src/platform_win32.c` (plus the platform-neutral `src/glob_match.c`), compiled
only for the Win64 target so the working POSIX [runner](runner.md) is untouched:

| POSIX primitive | Runner feature | Win32 port | Test target |
|---|---|---|---|
| `fork` / `waitpid` / `alarm` | per-test isolation + timeout | `CreateProcess` + `WaitForSingleObject` + `TerminateProcess` | `make win64-isolate-test` |
| `mmap` + `mprotect(PROT_NONE)` | guard-page allocator | `VirtualAlloc` + `VirtualProtect(PAGE_NOACCESS)` | `make win64-guard-test` |
| `poll` over children | the `-jN` parallel pool | `WaitForMultipleObjects` | `make win64-pool-test` |
| `sigaction` + `siglongjmp` | in-process crash-to-failure (`--no-fork`) | vectored exception handler + `__builtin_longjmp` | `make win64-seh-test` |
| `fnmatch` | `--filter` glob | portable matcher (`src/glob_match.c`) | `make win64-filter-test` |

All five are implemented and **verified under Wine** — each target above builds a
real PE with MinGW-w64 and runs it under `wine64` — and they join the
`asmtest-win64` image's `make win64-check` and the CI `win64` job. The two subtle
ones:

- **Isolated execution.** `asmtest_win32_run` / `_run_pool` spawn a test body in a
  child and classify it as OK (exit code captured), **CRASH** (an unhandled
  hardware exception — the child's exit code is the NTSTATUS code, e.g.
  `0xC0000005`), or **TIMEOUT** (`TerminateProcess` past the deadline). Process
  isolation gives crash containment without a fragile in-process dance; the pool
  keeps at most `jobs` children in flight, refilling a slot per
  `WaitForMultipleObjects` wake.
- **In-process crash-to-failure.** `asmtest_win32_guard` runs a body under a
  vectored exception handler that, on a fatal fault, redirects the thread to a
  landing pad that `__builtin_longjmp`s back — a minimal sp/fp/pc restore with no
  SEH unwinding, sidestepping the MinGW `longjmp`+unwind hazard. Recovery through
  frames lacking unwind data is best-effort; the forked path is the unconditional
  containment.

**Integration status.** A thin platform seam (`src/platform.h`) now backs the
runner: `src/asmtest.c` routes `--filter` through an `ASMTEST_FNMATCH` shim
(`fnmatch` on POSIX, `asmtest_glob_match` on Win32) and gates its guard-page
allocator under `!defined(_WIN32)`, with **no POSIX regression** — the library
and suites build and run identically on the host.

The execution-model re-route is **done**: `src/asmtest.c` now builds with MinGW and
runs as the Win64 runner itself. A Win32 `run_one` wraps each test body in the
per-test facility (vectored handler + watchdog) and maps the recovery reason to
fail/skip/crash/timeout; `main()` runs `--no-fork`, with the fork/pipe/poll
isolation, the parallel pool, the POSIX signal handlers, and the SysV
trampoline-driven helpers all gated to POSIX. `tests/win64/suite_win64.c` is a real
`TEST()` suite; `make win64-runner-test` (in `win64-check` / the CI `win64` job)
builds it with MinGW and runs it under Wine, where the runner discovers and runs
the suite, asserts real Win64 captures, and **contains a crashing and a hanging
test as reported failures while surviving**. Still on the POSIX-only side: a
forked/`-jN` execution mode on Win64 (the isolation primitives exist and are
tested, but the runner does not yet re-exec per test) and benchmarks.

## Caveats

- **Wine ≠ Windows at the edges.** For pure computation and ABI/capture testing
  Wine is faithful, but it is not a Windows host. For authoritative real-OS
  sign-off, add a `windows-latest` CI job running the same `nasm -f win64` suite.
- **x86-64 only.** On an AArch64 host the native lane does not apply; Win64-x64
  there stays [emulator](emulator.md)-only (Unicorn), matching the existing
  optional-emulator stance.
- **`_vec`/`_vec_n` use the vectorcall xmm convention.** The Win64 *default*
  convention passes `__m128` by reference; these variants deliberately model the
  `__vectorcall` xmm0–3 convention, the useful capture model for SIMD routines
  that read their inputs from xmm registers.
