# Native Win64 tier

asm-test can capture and assert a routine running under the **Microsoft x64
("Win64") ABI on real x86-64 silicon**, alongside the
[emulator](emulator.md)'s `emu_call_win64`, which runs Win64 bytes on a System V
host. The native tier exercises the real ABI on real hardware — and, crucially,
**without a Windows host**: it cross-compiles to a Windows PE and runs it under
Wine, or drives the trampoline directly via a compiler ABI attribute.

> Scope: this is the **capture** tier — `call` a routine through the real Win64
> ABI and snapshot its registers/flags/ABI-preservation. The framework's
> POSIX runner (per-test fork isolation, timeouts, guard pages) is not ported to
> Win32; the Win64 suite runs `--no-fork`. See the
> [implementation plan](https://github.com/wilvk/asm-test/blob/main/docs/plans/win64-native-tier-plan.md)
> for why, and what a full Win32 runner port would take.

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
as callee-saved** on the vector side.

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
