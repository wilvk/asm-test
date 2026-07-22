# AArch64 SVE wide-vector capture (Track D) — implementation

> **Sources.** Actioned from
> [post-v1-expansion-plan.md](../plans/post-v1-expansion-plan.md) (Track D —
> Wide-vector capture; the "SVE staged" heading and the "Still staged:
> AArch64 SVE" item) and
> [2026-07-04-plans-remaining-items.md](../analysis/2026-07-04-plans-remaining-items.md)
> (the "Wide-vector SVE" gate record, line 94). Written 2026-07-17. If this doc
> and a source disagree, this doc wins (sources may be stale); if the CODE and
> this doc disagree, re-verify before implementing.

## Why this work exists

The native capture tier can call an assembly routine through the real ABI and
hand the test its full vector state — but only up to 512-bit x86 (`vec128_t`,
`vec256_t`/AVX2, `vec512_t`/AVX-512). An AArch64 routine written with SVE
(Scalable Vector Extension: `z0..z31` vectors of implementation-defined length,
`p0..p15` predicates) cannot have that state captured at all today. This work
adds a scalable-vector type model, a GAS trampoline that marshals z-register
arguments and captures the whole z/predicate file, a `HWCAP_SVE` runtime probe
with clean self-skip, manifest pins, a corpus routine, and a QEMU vector-length
sweep lane — closing the last gap in Track D.

## What already exists (verified 2026-07-17)

Every claim below was checked against the working tree on 2026-07-17.

- **No SVE anywhere.** `grep -rniE 'sve|HWCAP' include/ src/` returns nothing:
  no `HWCAP_SVE`, no z-register symbols, no `getauxval` call sites in `src/` or
  `cli/`. The feature is genuinely absent, not half-landed.
- **The fixed-width idiom to mirror** lives in
  [include/asmtest.h](../../../include/asmtest.h): `vec256_t`/`vec512_t`
  lane-view unions with `ASMTEST_STATIC_ASSERT` size pins (lines 96–127), the
  runtime probes `asmtest_cpu_has_avx2`/`_avx512f` (declared 299–303,
  implemented in [src/asmtest.c](../../../src/asmtest.c) 461–498 with CPUID +
  XCR0 checks), the capture entry points `asm_call_capture_vec256`/`_vec512`
  (311–321), the self-skipping `ASM_VCALL256_*`/`ASM_VCALL512_*` macros
  (921–963), and `ASSERT_VEC256_EQ`/`ASSERT_VEC512_EQ` (1075–1088) backed by
  `asmtest_assert_vec512_eq` in src/asmtest.c (442–459).
- **The trampolines to extend**: [src/capture.s](../../../src/capture.s) (GAS,
  1820 lines) has the AArch64 AAPCS64 capture at lines 78–120 (stash
  out/fn on the stack, marshal `args[0..5] -> x0..x5`, `blr`, store results),
  `asm_call_capture_vec256` at 1105–1188 and `_vec512` at 1190–1288 — each with
  a `ret`-only stub in the `#else` branch "so the symbol resolves on other
  arches". The NASM twin [src/capture.asm](../../../src/capture.asm) (x86-64
  only, used under `ASM_SYNTAX=nasm`) has vec256 at 226–295 and vec512 at
  297–383. The last function in capture.s is `asm_bigstruct_x86`
  (ends line 1819), so SVE code appended at end-of-file can safely change the
  assembler `.arch` without affecting earlier code.
- **Manifest**: [scripts/gen-manifest.c](../../../scripts/gen-manifest.c)
  (203 lines) emits `asmtest_abi.json` via `make manifest`
  ([Makefile](../../../Makefile) line 480). It pins `vec128_t` and `vec256_t`
  (lines 71–81). Note: `vec512_t` is **not** in the manifest today (grep
  confirms) — mirror the `vec256_t` precedent for the new types and do not
  silently widen scope.
- **Corpus pattern**: [examples/simd.s](../../../examples/simd.s) holds
  `vec_add4f` (portable) plus x86-only `vec_add4d` (AVX2) and `vec_add8d`
  (AVX-512) inside `#if defined(__x86_64__)`;
  [examples/test_simd.c](../../../examples/test_simd.c) tests them inside the
  same guard (lines 48–82). Suites are auto-discovered: any
  `examples/test_foo.c` + `examples/foo.s` pair links via the Makefile
  `test_%` pattern rule (Makefile lines 53–71), so extending this pair needs
  **no Makefile change**. `tests/expect.sh` pins no simd test counts, so new
  arch-gated tests cannot break `make check`.
- **The arm64 lane**: `make docker-test DOCKER_PLATFORM=linux/arm64` runs the
  suite in an arm64 container ([mk/docker.mk](../../../mk/docker.mk) lines
  20–22, 37; [Dockerfile](../../../Dockerfile) header). On a non-arm64 host
  Docker runs it under binfmt qemu-user. **Empirically verified on the
  authoring host (x86-64 macOS, Docker Desktop, 2026-07-17):** an arm64
  container reports `AT_HWCAP` with bit 22 (`HWCAP_SVE`) **set**,
  `PR_SVE_GET_VL` = 64 bytes (512-bit VL), and `docker run -e
  QEMU_CPU=max,sve-max-vq=N` changes the VL — vq=1 → 16 bytes, vq=3 → 48 bytes
  (a non-power-of-two length), and
  `QEMU_CPU=max,sve-max-vq=16,sve-default-vector-length=-1` → 256 bytes (the
  architectural max). So on this class of host the SVE path *executes* under
  TCG before any real hardware exists. On an Apple-silicon host the same lane
  runs natively with **no** SVE (see Constraints) and must self-skip.
- **Baseline proof before touching anything**: `make test && make check` exits
  0 on the host (per-test `ok` lines, `expect.sh` TAP output ending with no
  `not ok`), and `make docker-test DOCKER_PLATFORM=linux/arm64` does the same
  in the arm64 container. Run both first; if either is red, stop and fix that
  before starting.

## Design positions (read before T1)

1. **Max-size container, VL-probed at runtime.** SVE's vector length (VL) is
   implementation-defined, 16–256 bytes (the Linux kernel guarantees
   `16 <= VLmax <= 256`). Unlike `vec256_t`/`vec512_t`, a fixed-true-width type
   is impossible, so the model is a **256-byte container** (`svec_t`) of which
   only the low `asmtest_sve_vl()` bytes are live, plus a 32-byte predicate
   container (`spred_t`, max PL = VLmax/8). Assertions compare only the live
   bytes. This keeps the shipped `type → _Static_assert → manifest` idiom
   (fixed sizeof) while staying VL-agnostic.
2. **No scaled immediates in the trampoline.** SVE `LDR/STR` immediates are
   scaled by VL (`MUL VL`), but the C-side arrays have a *fixed* container
   stride (256/32 bytes). The trampoline therefore uses only `[xN]` (zero
   offset) addressing with explicit `add` address arithmetic — which also
   sidesteps the predicate-immediate scaling question entirely (see Research
   notes caveats).
3. **Capture the vector/predicate file only.** Mirroring `_vec256`/`_vec512`:
   GP registers, flags, and ABI sentinels stay on the 128-bit
   `asm_call_capture_vec` path. The z return value is `z[0]` (AAPCS64 §6.1.3).
4. **Linux-only, aarch64-only, runtime-gated.** Apple silicon has no
   non-streaming SVE (SVE instructions outside SME streaming mode are
   inaccessible), so macOS arm64 takes the stub/skip path. The real trampoline
   body is `#if defined(__aarch64__) && defined(__linux__)`; everywhere else a
   `ret` stub keeps the symbol resolvable, exactly like the vec256 stub.

## Tasks

### T1 — Add the scalable-vector type model and runtime SVE probe  (S, depends on: none)

**Goal.** `svec_t`/`spred_t` exist with layout pins, and
`asmtest_cpu_has_sve()`/`asmtest_sve_vl()` report SVE presence and vector
length, returning 0 everywhere SVE is absent.

**Steps.**
1. In [include/asmtest.h](../../../include/asmtest.h), directly after the
   `vec512_t` block (after line 127), add `svec_t` and `spred_t` with the same
   lane-view union idiom and comments, plus size pins:
2. After the `asm_call_capture_vec512` declaration (line 321), declare the
   probes (implementation next step). The capture entry point
   `asm_call_capture_sve` is declared in T2 alongside its trampoline, so add
   only the two probe declarations here.
3. In [src/asmtest.c](../../../src/asmtest.c), extend the CPU-probe block
   (lines 461–498): keep the x86 branch as is, add an
   `#if defined(__aarch64__) && defined(__linux__)` branch, and make the final
   `#else` cover everything else (including macOS arm64 and Win64).
4. `make test && make check` — still green on the x86-64 host (new code
   compiles to the constant-0 branch).
5. `make fmt` (clang-format is CI-gated via `fmt-check`).

**Code.** Header additions:

```c
/* One SVE scalable vector register (z0..z31). VL is implementation-defined
 * (16..256 bytes on Linux), so this is a MAX-SIZE container: only the low
 * asmtest_sve_vl() bytes are live after asm_call_capture_sve; the rest are
 * untouched. Same lane-view idiom as vec128_t/vec256_t/vec512_t. */
typedef union {
    unsigned char u8[256];
    uint32_t u32[64];
    uint64_t u64[32];
    float f32[64];
    double f64[32];
} svec_t;
ASMTEST_STATIC_ASSERT(sizeof(svec_t) == 256, "svec_t is 256 bytes (SVE VLmax)");

/* One SVE predicate register (p0..p15): one bit per vector byte, so PL = VL/8
 * (max 32 bytes). Only the low asmtest_sve_vl()/8 bytes are live. */
typedef union {
    unsigned char u8[32];
    uint64_t u64[4];
} spred_t;
ASMTEST_STATIC_ASSERT(sizeof(spred_t) == 32, "spred_t is 32 bytes (SVE PLmax)");
```

Probe declarations (place beside the AVX probes so the "self-skip, never
fault" comment block at lines 299–301 covers them):

```c
/* AArch64 Linux only; 0 elsewhere (Apple silicon has no non-streaming SVE). */
int asmtest_cpu_has_sve(void);
/* SVE vector length in BYTES via rdvl (16..256); 0 when SVE is absent. */
unsigned long asmtest_sve_vl(void);
```

src/asmtest.c implementation (inside the existing probe section):

```c
#if defined(__aarch64__) && defined(__linux__)
#include <sys/auxv.h>
#ifndef HWCAP_SVE
#define HWCAP_SVE (1UL << 22) /* linux uapi asm/hwcap.h */
#endif
int asmtest_cpu_has_sve(void) {
    return (getauxval(AT_HWCAP) & HWCAP_SVE) != 0;
}
extern unsigned long asmtest_sve_rdvl(void); /* capture.s (T2) */
unsigned long asmtest_sve_vl(void) {
    return asmtest_cpu_has_sve() ? asmtest_sve_rdvl() : 0;
}
#else
int asmtest_cpu_has_sve(void) { return 0; }
unsigned long asmtest_sve_vl(void) { return 0; }
#endif
```

Note the non-Linux branch never references `asmtest_sve_rdvl`, so nothing new
must link on x86/macOS until T2 provides the stubs anyway.

**Tests.** No new test binary yet (T4 owns the executable surface). Manual
verification: `make test && make check` green on the host; additionally
`cc -std=c11 -Iinclude -fsyntax-only include/asmtest.h` still passes (the
header stays ISO-C clean — the repo's `check-header-portability` target guards
this too).

**Docs.** Internal-only at this stage; user-facing docs land in T7 once the
feature is callable.

**Done when.**
- `make test && make check` exit 0 on the x86-64 host.
- `grep -n 'svec_t\|asmtest_cpu_has_sve' include/asmtest.h` shows the types,
  pins, and probe declarations.
- `make fmt-check` reports no drift.

### T2 — Implement the SVE capture trampoline and cross-arch stubs  (M, depends on: T1)

**Goal.** `asm_call_capture_sve` marshals z/p/x arguments per AAPCS64, calls
the routine, and captures the full `z0..z31` + `p0..p15` file; a `ret` stub
resolves the symbol on every non-SVE target, in both assembler backends.

**Steps.**
1. Declare in include/asmtest.h (with the other capture entry points,
   after line 321):

   ```c
   /* The SVE analog of asm_call_capture_vec256: marshal up to 8 scalable
    * vector args into z0..z7 and up to 4 predicate args into p0..p3 (AAPCS64
    * §6.1.3/§6.1.4), call fn, then capture the WHOLE z file (z0..z31) into
    * z[0..31] and predicate file (p0..p15) into p[0..15]. Only the low VL
    * (resp. VL/8) bytes of each slot are written — the container tails are
    * untouched. z[0] = the scalable-vector return. AArch64 Linux + SVE only —
    * gate with asmtest_cpu_has_sve(), or use ASM_SVCALL_*, which self-skip.
    * Captures the vector/predicate file only; use the 128-bit path for
    * GP/flags capture. iargs has 6 slots, zargs 8, pargs 4 (none may be NULL). */
   void asm_call_capture_sve(svec_t *z, spred_t *p, void *fn,
                             const long *iargs, const svec_t *zargs,
                             const spred_t *pargs);
   ```
2. Append the trampoline **at the end of** [src/capture.s](../../../src/capture.s)
   (after `asm_bigstruct_x86`, line 1819) — last on purpose: the
   `.arch armv8-a+sve` directive persists to end-of-file, and nothing SVE may
   leak into the plain-ARMv8 functions above. Mirror the comment style of the
   vec256 block (lines 1105–1115).
3. Append `asmtest_sve_rdvl` after it.
4. Add a `ret` stub for both symbols to [src/capture.asm](../../../src/capture.asm)
   (NASM backend, x86-64-only file — append after `asm_call_capture_vec512`,
   line 383, using its stub-comment style; for `asmtest_sve_rdvl` clear `eax`
   first so a misuse returns 0, not garbage).
5. `make test && make check`, then `make ASM_SYNTAX=nasm test` (x86 host), then
   `make docker-test DOCKER_PLATFORM=linux/arm64` (assembles + links the real
   body; the tests that *call* it arrive in T4).

**Code.** capture.s addition (argument registers per AAPCS64: `z -> x0`,
`p -> x1`, `fn -> x2`, `iargs -> x3`, `zargs -> x4`, `pargs -> x5`):

```asm
ASM_FUNC asm_call_capture_sve

#if defined(__aarch64__) && defined(__linux__)
    .arch   armv8-a+sve
    sub     sp, sp, #48
    stp     x29, x30, [sp, #0]
    mov     x29, sp
    str     x0, [sp, #16]           /* stash z out across the call */
    str     x1, [sp, #24]           /* stash p out across the call */
    str     x2, [sp, #32]           /* stash fn  across the call */

    /* Predicate args pargs[0..3] -> p0..p3. Fixed 32-byte container stride:
     * zero-offset [xN] addressing only (no VL/PL-scaled immediates). */
    mov     x9, x5
    ldr     p0, [x9]
    add     x9, x9, #32
    ldr     p1, [x9]
    add     x9, x9, #32
    ldr     p2, [x9]
    add     x9, x9, #32
    ldr     p3, [x9]

    /* Vector args zargs[0..7] -> z0..z7 (fixed 256-byte stride; each ldr
     * moves only VL bytes). */
    mov     x9, x4
    ldr     z0, [x9]
    add     x9, x9, #256
    ldr     z1, [x9]
    /* ... same add/ldr pairs through z7 ... */

    /* Integer args iargs[0..5] -> x0..x5 (mirrors asm_call_capture, line 94). */
    mov     x9, x3
    ldp     x0, x1, [x9, #0]
    ldp     x2, x3, [x9, #16]
    ldp     x4, x5, [x9, #32]

    ldr     x10, [sp, #32]          /* fn */
    blr     x10

    /* Capture the full z file: z0..z31 -> z[0..31], 256-byte stride. */
    ldr     x9, [sp, #16]
    str     z0, [x9]
    add     x9, x9, #256
    str     z1, [x9]
    /* ... through z31 ... */

    /* Capture the predicate file: p0..p15 -> p[0..15], 32-byte stride. */
    ldr     x10, [sp, #24]
    str     p0, [x10]
    add     x10, x10, #32
    str     p1, [x10]
    /* ... through p15 ... */

    ldp     x29, x30, [sp, #0]
    add     sp, sp, #48
    ret
#else
/* SVE is AArch64-Linux-only; a stub so the symbol resolves on other targets
 * (x86-64, macOS arm64 — Apple silicon has no non-streaming SVE). Never
 * called: asmtest_cpu_has_sve() is false there, so the macros self-skip. */
    ret
#endif

ASM_ENDFUNC asm_call_capture_sve

/* unsigned long asmtest_sve_rdvl(void) — VL in bytes (rdvl x0, #1). */
ASM_FUNC asmtest_sve_rdvl
#if defined(__aarch64__) && defined(__linux__)
    .arch   armv8-a+sve
    rdvl    x0, #1
    ret
#elif defined(__aarch64__)
    mov     x0, #0
    ret
#else
    xorl    %eax, %eax
    ret
#endif
ASM_ENDFUNC asmtest_sve_rdvl
```

ABI notes to preserve in comments: the trampoline itself takes no SVE
arguments, so per AAPCS64 it owes its caller only the usual d8–d15 low-64-bit
preservation — and it touches neither, so no vector save/restore is needed.
The routine *under test* does take SVE arguments, so per AAPCS64 the entire
`z8–z23` and `p4–p15` are callee-saved **for it** — which is why the corpus
routine (T4) must scratch only `p0–p3`/`z0–z7`.

**Tests.** Compile/link-level in this task (all three commands in step 5 exit
0); behavior is exercised by T4/T6. A failure here looks like an assembler
error (`unknown mnemonic ldr z0…` means the `.arch` directive is missing or
misplaced) or an unresolved `asm_call_capture_sve` under `ASM_SYNTAX=nasm`
(means the capture.asm stub was skipped).

**Docs.** Internal-only; T7 owns user-facing pages.

**Done when.**
- `make test && make check` and `make ASM_SYNTAX=nasm test` exit 0 on x86-64.
- `make docker-test DOCKER_PLATFORM=linux/arm64` exits 0.
- `nm build/capture.o | grep -i sve` shows both symbols on both arches.

### T3 — Add self-skipping call macros and VL-aware assertions  (S, depends on: T1, T2)

**Goal.** `ASM_SVCALL_1/_2` call the trampoline and SKIP cleanly without SVE;
`ASSERT_SVEC_EQ`/`ASSERT_SPRED_EQ` compare exactly the live VL (resp. PL)
bytes.

**Steps.**
1. In include/asmtest.h after `ASM_VCALL512_2` (line 963), add the macros,
   mirroring the `ASM_VCALL512_*` shape (lines 946–963) — the gate is
   `asmtest_cpu_has_sve()`, the skip string is
   `"SVE not available on this host"`, and `pargs` is a zeroed `spred_t[4]`
   (all-false predicates; a routine needing predicate *arguments* calls
   `asm_call_capture_sve` directly):

   ```c
   #define ASM_SVCALL_2(z, p, fn, v0, v1)                                     \
       do {                                                                   \
           if (!asmtest_cpu_has_sve())                                        \
               SKIP("SVE not available on this host");                        \
           long asmtest_ia_[6] = {0};                                        \
           svec_t asmtest_za_[8] = {(v0), (v1)};                             \
           spred_t asmtest_pa_[4] = {{{0}}};                                 \
           asm_call_capture_sve((z), (p), (void *)(fn), asmtest_ia_,         \
                                asmtest_za_, asmtest_pa_);                   \
       } while (0)
   ```

   (`ASM_SVCALL_1` is the single-arg variant.)
2. Declare the helpers beside `asmtest_assert_vec512_eq` (line 613) and add
   the assertion macros after `ASSERT_VEC512_EQ` (line 1088):

   ```c
   /* VL-aware: compares the low asmtest_sve_vl() bytes of z-slot idx. */
   #define ASSERT_SVEC_EQ(z, idx, expect_ptr)                                 \
       asmtest_assert_svec_eq(__FILE__, __LINE__, #idx, (z)[idx].u8,          \
                              (const unsigned char *)(expect_ptr))
   /* PL-aware: compares the low asmtest_sve_vl()/8 bytes of p-slot idx. */
   #define ASSERT_SPRED_EQ(p, idx, expect_ptr)                                \
       asmtest_assert_spred_eq(__FILE__, __LINE__, #idx, (p)[idx].u8,         \
                               (const unsigned char *)(expect_ptr))
   ```
3. Implement both in src/asmtest.c next to `asmtest_assert_vec512_eq`
   (lines 442–459): same walk-until-first-diff shape, but the bound is
   `asmtest_sve_vl()` (resp. `/8`) instead of 64, the failure message names
   the VL (`"ASSERT_SVEC_EQ(z[%s], vl=%lu): first diff at byte %zu …"`), and
   the hexdump uses the existing `hexdump_window(buf, cap, p, start, end)`
   helper (src/asmtest.c line 152) with a 64-byte window aligned to contain
   the first differing byte (`start = i & ~(size_t)63`), since a 256-byte VL
   no longer fits the fixed dump buffers. If the bound is 0 (no SVE), fail
   with a distinct message — asserting SVE state without SVE is a test bug,
   not a comparison.
4. `make test && make check && make fmt`.

**Code.** Shown inline in Steps above: the `ASM_SVCALL_1`/`_2` call macros, the
`ASSERT_SVEC_EQ`/`ASSERT_SPRED_EQ` assertion macros, and the
`asmtest_assert_svec_eq`/`_spred_eq` walk-until-first-diff helpers in
src/asmtest.c.

**Tests.** T4 exercises the pass path; the negative path (a deliberate
mismatch fails with the VL-aware message) is verified manually on the T6 lane
by temporarily flipping one expected lane — record the observed message in the
PR description. (tests/expect.sh pins no vector-assert strings, verified, so
no self-test extension is required.)

**Docs.** Internal-only until T7.

**Done when.**
- Host + arm64-container builds green (same three commands as T2).
- `make fmt-check` clean.

### T4 — Add the SVE corpus routine and VL-agnostic tests  (S, depends on: T2, T3)

**Goal.** A real SVE routine in the corpus is executed and lane-asserted at
whatever VL the host provides, and the suite self-skips cleanly without SVE.

**Steps.**
1. In [examples/simd.s](../../../examples/simd.s), append after the x86 block
   (line 42), gated `#if defined(__aarch64__) && defined(__linux__)`, with
   `.arch armv8-a+sve` first (this file has no code after it, mirroring the
   capture.s end-of-file rule):

   ```asm
   /*
    * svec sve_addd(svec a, svec b);  lane-wise add of VL/8 64-bit doubles
    * (SVE): a -> z0, b -> z1, result -> z0. AArch64 Linux only; the capture
    * path is HWCAP_SVE-gated and the test self-skips where SVE is absent.
    * p3 is the scratch governing predicate: p0-p3 are argument/caller-saved
    * registers, while p4-p15 are callee-saved HERE because this routine takes
    * SVE arguments (AAPCS64) — so only p0-p3 may be clobbered.
    */
   ASM_FUNC sve_addd
       ptrue   p3.d
       fadd    z0.d, p3/m, z0.d, z1.d
       ret
   ASM_ENDFUNC sve_addd
   ```
2. In [examples/test_simd.c](../../../examples/test_simd.c), append an
   `#elif defined(__aarch64__) && defined(__linux__)` arm to the existing
   `#if defined(__x86_64__)` block (lines 48–82), mirroring the AVX2 test
   comment style:

   ```c
   extern void sve_addd(void); /* svec sve_addd(svec a, svec b), SVE */

   TEST(simd, sve_adds_doubles_at_any_vl) {
       svec_t a = {{0}}, b = {{0}};
       for (int i = 0; i < 32; i++) { /* fill to VLmax so ANY VL is covered */
           a.f64[i] = (double)(i + 1);
           b.f64[i] = 10.0 * (double)(i + 1);
       }
       svec_t z[32];
       spred_t p[16];
       ASM_SVCALL_2(z, p, sve_addd, a, b); /* self-skips without SVE */

       unsigned long vl = asmtest_sve_vl(); /* bytes; >= 16 once here */
       ASSERT_UGE(vl, 16);
       for (unsigned long i = 0; i < vl / 8; i++)
           ASSERT_DEQ(z[0].f64[i], 11.0 * (double)(i + 1));

       svec_t expect = {{0}};
       for (unsigned long i = 0; i < vl / 8; i++)
           expect.f64[i] = 11.0 * (double)(i + 1);
       ASSERT_SVEC_EQ(z, 0, expect.u8); /* compares exactly vl bytes */

       /* ptrue p3.d in the routine: one predicate bit per vector byte, so a
        * .d all-true pattern reads 0x01 in every live predicate byte. */
       for (unsigned long i = 0; i < vl / 8; i++)
           ASSERT_UEQ(p[3].u8[i], 0x01);
   }
   #endif
   ```
3. `make test` on the x86-64 host: suite count unchanged (the arm is compiled
   out). `make docker-test DOCKER_PLATFORM=linux/arm64`: on a binfmt-qemu host
   the new test **runs and passes** at VL=64; on a native-arm64 non-SVE host it
   reports a skip with reason `SVE not available on this host`.

**Code.** As above — no Makefile edits (the `test_simd`/`simd.s` pair already
links via the pattern rule).

**Tests.** This *is* the test. Pass: `ok`-style line for
`simd.sve_adds_doubles_at_any_vl` in the arm64 lane. Fail: a lane mismatch
prints the VL-aware `ASSERT_SVEC_EQ` diff (T3); a wrong predicate pattern
prints the `ASSERT_UEQ` hex compare. Skip: the SKIP reason string above.

**Docs.** T7.

**Done when.**
- x86-64 host: `make test && make check` green, no new test listed.
- `make docker-test DOCKER_PLATFORM=linux/arm64` green with the SVE test
  either passing (qemu exposes SVE — the authoring-host case) or skipping with
  the exact reason string (native arm64 without SVE).

### T5 — Pin svec_t/spred_t in the ABI manifest  (S, depends on: T1)

**Goal.** `asmtest_abi.json` carries the new containers so a binding generator
can mirror them without transcribing offsets.

**Steps.**
1. In [scripts/gen-manifest.c](../../../scripts/gen-manifest.c), after the
   `vec256_t` block (lines 77–81), add:

   ```c
   BEGIN(svec_t); /* SVE scalable-vector container: fixed 256-byte (VLmax)
                     box; live bytes = asmtest_sve_vl() at runtime */
   FIELD(svec_t, u8);
   FIELD(svec_t, u64);
   FIELD(svec_t, f64);
   end_struct();

   BEGIN(spred_t); /* SVE predicate container: fixed 32-byte (PLmax) box */
   FIELD(spred_t, u8);
   end_struct();
   ```
2. `make manifest` — regenerates `asmtest_abi.json` at the repo root. This file
   is a **generated artifact**, not source: `.gitignore` lists it under
   "Generated bindings-substrate artifacts (Track 0)" and `git ls-files` shows
   it untracked, so there is nothing to `git add`, commit, or `git diff` —
   verify the new structs landed with the Python check in **Tests** below. The
   Win64 variant needs no change, but run `make manifest-win64` once to confirm
   it still builds (the types are unconditional, like `vec256_t`).

**Code.** As above. These are plain C unions, so the block is unconditional
and emits identically on every host — the *scalable* part is runtime data
(`asmtest_sve_vl`), deliberately not baked into the layout manifest.

**Tests.** `make manifest && python3 -c "import json;
d=json.load(open('asmtest_abi.json'));
print([s['name'] for s in d['asmtest_abi']['structs']])"` lists `svec_t` and
`spred_t` with `"size": 256` / `"size": 32`. A failure looks like a compile
error in gen-manifest (header mismatch) or the names missing from the list.

**Docs.** None beyond T7's changelog line (the manifest is itself the
machine-readable doc).

**Done when.**
- The Python check in **Tests** lists `svec_t` (`"size": 256`) and `spred_t`
  (`"size": 32`) in the regenerated `asmtest_abi.json` (a gitignored build
  artifact — nothing to commit or diff).
- `make manifest manifest-win64` both exit 0.

### T6 — Add the QEMU vector-length sweep lane (docker-sve-sweep)  (M, depends on: T4)

**Goal.** A make target executes the SVE suite at several VLs — including a
non-power-of-two — under qemu binfmt, so VL-assumption bugs are flushed out
before any SVE silicon is available, and the lane self-skips (with a printed
reason) only where neither qemu interposition nor SVE hardware exists.

**Steps.**
1. In [mk/docker.mk](../../../mk/docker.mk), after the `docker-asm` rule
   (lines 46–47), add a dedicated lane mirroring the `docker-dataflow-attach`
   comment-plus-rule pattern (comment lines 49–55, `.PHONY` + rule lines
   56–61):

   ```make
   # --- docker-sve-sweep: pre-hardware SVE validation under qemu binfmt --------
   # Runs the simd suite in an arm64 container at several SVE vector lengths by
   # steering qemu-user's CPU through the QEMU_CPU env var (read by the binfmt
   # interpreter): sve-max-vq=N caps VL at N*16 bytes and
   # sve-default-vector-length=-1 selects the max, so the sweep covers VQ 1
   # (128-bit), 3 (384-bit — non-power-of-two), 8 and 16 (2048-bit). Verified
   # 2026-07-17 on an x86-64 Docker Desktop host: default VL=64B; vq=1 -> 16B,
   # vq=3 -> 48B, vq=16 + default-vector-length=-1 -> 256B. On a NATIVE arm64
   # host there is no qemu to steer: QEMU_CPU is ignored and the test itself
   # self-skips without SVE silicon (Apple silicon has none) — that skip line
   # is this lane's honest output there, not a failure. TCG timings are
   # meaningless; never point bench targets at this lane.
   DOCKER_SVE_IMAGE ?= asmtest-ci-arm64
   DOCKER_SVE_VQS   ?= 1 3 8 16
   .PHONY: docker-sve-sweep
   docker-sve-sweep:
   	$(DOCKER) build --platform linux/arm64 --build-arg BASE=$(DOCKER_BASE) \
   	  -t $(DOCKER_SVE_IMAGE) .
   	set -e; for vq in $(DOCKER_SVE_VQS); do \
   	  echo "== docker-sve-sweep: QEMU_CPU=max,sve-max-vq=$$vq =="; \
   	  $(DOCKER) run --rm --platform linux/arm64 \
   	    -e QEMU_CPU=max,sve-max-vq=$$vq,sve-default-vector-length=-1 \
   	    $(DOCKER_SVE_IMAGE) \
   	    sh -c 'make build/test_simd >/dev/null && ./build/test_simd'; \
   	done
   ```
2. Add `docker-sve-sweep` to the `.PHONY` docker list and one line to the
   `make help` Docker section ([Makefile](../../../Makefile), the
   `docker-drext-probe` line is the style to copy):
   `@echo '  docker-sve-sweep           SVE suite at VQ 1/3/8/16 under qemu binfmt (arm64)'`.
3. Run `make docker-sve-sweep` on the host. Each iteration prints the banner,
   then the test_simd run; the SVE test must PASS at every VQ (the per-lane
   loop count differs per VL, which is exactly the sweep's point).
4. This lane needs **no new Dockerfile and no new dependency**: the existing
   [Dockerfile](../../../Dockerfile) already documents arm64-via-binfmt use,
   and qemu comes from the host's binfmt setup (Docker Desktop ships it; bare
   Linux: `docker run --privileged tonistiigi/binfmt` once, as the Dockerfile
   header says). No version pin is possible for the host-side qemu — record
   the qemu behavior observed (VLs reached) in the PR instead, and note that
   no QEMU version is pinned anywhere in this tree today.

**Code.** As above; no C changes.

**Tests.** The lane is the test. Pass: four banners, four green test_simd
runs (different VLs). Fail: any nonzero exit propagates (`set -e`; no
`;`-chaining — the Track A lesson about discarded exit statuses). Skip: on a
native-arm64 host the suite prints the SKIP reason and exits 0 — that is the
recorded gate, not a green light for VL correctness (say so in the PR).

**Docs.** T7 adds the lane to the troubleshooting/wide-vector docs.

**Done when.**
- `make docker-sve-sweep` exits 0 on the authoring-class host with the SVE
  test passing at VQ 1, 3, 8, 16.
- `make help` lists the target.
- On a native-arm64 non-SVE host the lane exits 0 with the printed skip.

### T7 — User-facing docs and changelog  (S, depends on: T4)

**Goal.** A user can discover and use SVE capture from the published docs, and
the stale "SVE is not yet wired" text is gone.

**Steps.**
1. [docs/guides/floating-point-simd.md](../../guides/floating-point-simd.md):
   after the AVX2 section (line 99 heading), add
   `### Wide vectors — SVE (scalable, AArch64 Linux)` documenting `svec_t`/
   `spred_t`, `ASM_SVCALL_*`, `asmtest_cpu_has_sve`/`asmtest_sve_vl`,
   VL-agnostic assertion style (loop to `vl/8`; `ASSERT_SVEC_EQ` compares live
   bytes only), the zeroed-predicate-args note, and the platform story (Linux
   only; Apple silicon has no non-streaming SVE, so macOS arm64 self-skips;
   `make docker-sve-sweep` for pre-hardware VL sweeps). Then **rewrite the
   callout at lines 124–131**: keep the AVX-512 and emulator sentences, delete
   "**AArch64 SVE is not yet wired.**", and point at the new section instead.
2. [docs/reference/api-reference.md](../../reference/api-reference.md): add an
   `ASM_SVCALL_1/_2` row beside the `ASM_VCALL512` row (line 32), the
   `asm_call_capture_sve` + probe signatures beside the vec512 block (lines
   101–104), and `asm_call_capture_sve` to the array-form capture enumeration
   (line ~200).
3. [docs/getting-started/examples.md](../../getting-started/examples.md): add
   the `sve_addd` snippet beside the `vec_add4d` one (lines 361–365 style).
4. [CHANGELOG.md](../../../CHANGELOG.md), under `## [Unreleased]` / `### Added`
   (one entry): SVE scalable-vector capture — `svec_t`/`spred_t`,
   `asm_call_capture_sve`, `ASM_SVCALL_*`, HWCAP probe, manifest pins, corpus
   routine, and the `docker-sve-sweep` QEMU lane; hardware sign-off tracked in
   T8. Link this doc via a GitHub blob URL (published pages may link into
   `docs/internal` only that way — repo convention).
5. `make docker-docs` (Sphinx builds with `-W`, so a broken link fails the
   build).

**Code.** None.

**Tests.** `make docker-docs` exits 0;
`grep -rn 'not yet wired' docs/guides/floating-point-simd.md` returns nothing.

**Docs.** This task *is* the docs task.

**Done when.**
- Docs build green; the SVE section renders; changelog entry present.

### T8 — Real-silicon execution sign-off  (S, depends on: T6) — **DONE 2026-07-22**

**Goal.** The trampoline is *executed* (not just assembled and TCG-emulated)
on real SVE silicon, and Track D's status flips to done.

> **The gate was never real — the SVE host was already in CI.** This task was
> written as hardware-gated on the belief that "hosts are x86-64 Zen 5 and
> non-SVE arm64". That belief was wrong about the hosted **`ubuntu-24.04-arm`**
> runner: it is Azure Cobalt 100 / **Neoverse-N2**, whose `/proc/cpuinfo`
> `Features` carries `sve sve2 sveaes svebitperm svesha3 svesm4 svei8mm svebf16`
> at `sve_default_vector_length` = **16 bytes**. The suite had therefore been
> *executing* the SVE trampoline on real silicon in every CI run — the evidence
> was sitting unread in the `test (ubuntu-24.04-arm)` log (`ok N -
> simd.sve_adds_doubles_at_any_vl`, `0 skipped`). Measured live 2026-07-22:
>
> ```
> Linux 6.17.0-1020-azure aarch64          # kernel
> CPU part : 0xd49                         # Neoverse-N2
> Features : … sve … sve2 sveaes svebitperm svesha3 svesm4 … svei8mm svebf16
> /proc/sys/abi/sve_default_vector_length = 16    # VL = 16 B (128-bit)
> make WERROR=1 test   → ok 5 - simd.sve_adds_doubles_at_any_vl (0 skipped)
> make WERROR=1 check  → # 57 passed, 0 failed
> ```
>
> Before believing a hardware gate, check the CI fleet: "no such host in this
> environment" is a claim about the runner matrix, not only about the desk.

**Steps.**
1. ~~Obtain an AArch64+SVE Linux host~~ — **done: the hosted
   `ubuntu-24.04-arm` runner is one** (Neoverse-N2, VL=16 B). Graviton3/3E/4
   (VL=32B), NVIDIA Grace (VL=16B) or A64FX (VL=64B) would additionally cover
   wider *native* VLs, but the sign-off this task asks for — the trampoline
   executing on silicon rather than under TCG — is met on the hosted runner.
   A remaining honest gate: no native VL **other than 16 B** has been executed
   on hardware (the 48/128/256 B legs remain qemu-emulated).
2. **Done** — `make WERROR=1 test && make WERROR=1 check` ran green on that
   runner with the SVE test *executing*, and the silicon facts are recorded
   above (kernel, CPU part, HWCAP `sve`, VL).
3. **Turn the one-off into a standing gate** (this is the part that keeps the
   sign-off from rotting): the `test` job in `.github/workflows/ci.yml` gains
   an arm64-only **"SVE silicon sign-off (must execute, not skip)"** step that
   prints `uname -srm` / `CPU part` / VL every run and then asserts
   `^ok [0-9]* - simd\.sve_adds_doubles_at_any_vl$` — i.e. the line WITHOUT a
   `# SKIP` suffix. `ASM_SVCALL_*` self-skipping is correct behaviour on a
   non-SVE host, which is exactly why it is dangerous here: a runner-fleet
   change or a regressed `HWCAP_SVE` probe would keep the leg green while
   silently retiring the only real-silicon validation the SVE path has.
4. Update [post-v1-expansion-plan.md](../plans/post-v1-expansion-plan.md)
   Track D: "SVE staged" → done, naming the host and VL validated (the plan
   header instructs tracks to update it as they land); update the CHANGELOG
   entry's sign-off note; update the gate line in
   [2026-07-04-plans-remaining-items.md](../analysis/2026-07-04-plans-remaining-items.md).

**Code.** No source change was needed — the trampoline executed correctly on
first contact with silicon. CI gains the sign-off step above; the stale
"only qemu-user under TCG exposes SVE here" comment in `examples/test_simd.c`
is corrected.

**Tests.** The suite run on the SVE host is the test; the observable is the
`ok` line with `0 skipped` at a VL (16 B) the qemu sweep produces only by
steering `QEMU_CPU`, now produced natively.

**Docs.** The two status updates + changelog note in step 4.

**Done when.**
- ~~A dated note in the plan names the silicon, kernel, and VL, with~~
  `make test && make check` green there — **met 2026-07-22** (Neoverse-N2,
  Linux 6.17.0-1020-azure, VL=16 B; `check` 57/0).
- The CI step above fails the arm64 `test` leg if that line ever self-skips,
  so the claim stays true rather than being re-asserted from memory.
- Still honestly gated: a **native VL other than 16 B** (Graviton3 at 32 B,
  A64FX at 64 B). The 48/128/256 B legs are qemu TCG and are reported as such.

## Task order & parallelism

- Critical path: **T1 → T2 → T3 → T4 → T6 → T8**.
- T5 (manifest) depends only on T1 and can run in parallel with T2–T4.
- T7 (docs) depends on T4 and can run in parallel with T6.
- Two people: one takes T1→T2 (asm-heavy), the other T3/T5 once T1 merges,
  then converge on T4.

```
T1 ──► T2 ──► T3 ──► T4 ──► T6 ──► T8
  └──► T5            └──► T7
```

## Constraints & gates

- ~~**Hardware gate (real):** executing on SVE silicon (T8).~~ **CLOSED
  2026-07-22** — the hosted `ubuntu-24.04-arm` runner IS SVE silicon
  (Neoverse-N2, VL=16 B), so the execution sign-off runs in CI on every push
  and is asserted not to self-skip. What remains gated is narrower and stated
  as such: a native VL other than 16 B (Graviton3 32 B / A64FX 64 B). Per
  CLAUDE.md, hardware is a legitimate self-skip gate — but check the CI runner
  matrix before recording one: this gate was self-inflicted for three days.
  Everything else here is installable/runnable today and must not self-skip:
  the QEMU sweep lane executes on any host with binfmt qemu.
- **Apple silicon is not an SVE host.** Apple's SME implementation makes SVE
  accessible only inside streaming mode; plain SVE instructions are
  inaccessible (SIGILL) on macOS arm64. The probe returning 0 there is
  correct behavior, not a missing feature.
- **qemu-user limits** (already recorded in
  [troubleshooting.md](../../reference/troubleshooting.md) line 97 and
  [native-tracing.md](../../guides/tracing/native-tracing.md) line 447): no
  ptrace tracer/tracee, no single-step, meaningless timings. The SVE simd
  suite touches none of those; do not extend the sweep lane to trace/bench
  targets.
- **No new pinned third-party artifact.** The sweep uses the host's binfmt
  qemu (unpinnable from inside the repo; no QEMU version is pinned anywhere in
  the tree today) and the existing `ubuntu:24.04`-based CI image — nothing to
  add to `scripts/third-party-digests.txt`.
- **Formatting/CI:** all C changes pass `make fmt-check`; header stays ISO-C
  (`check-header-portability`); the bindings parity gate
  (`scripts/check-bindings-parity.sh`) polices only the native-trace tier
  headers, so adding `asm_call_capture_sve` to `asmtest.h` does **not**
  obligate ten binding wrappers in this change (verified: `TIER_HEADERS` lists
  only the hwtrace/drtrace/trace_auto/ptrace/codeimage headers).

## Research notes (verified 2026-07-17)

- **AAPCS64 SVE calling convention** (Arm abi-aa, aapcs64.rst): z0–z7 pass and
  return scalable vector values (§6.1.3); p0–p3 pass and return predicates
  (§6.1.4). If a subroutine takes/returns scalable vector or predicate values,
  the **entire** z8–z23 and p4–p15 are callee-saved; otherwise only the low 64
  bits of z8–z15 (the ordinary d8–d15 rule) are.
  <https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst>
- **VL-agnostic instructions** (Arm A64 ISA via the Stanford mirror of Arm's
  machine-readable XML): `STR <Zt>, [<Xn|SP>{, #imm, MUL VL}]` stores the full
  VL bytes, immediate scaled by VL
  (<https://www.scs.stanford.edu/~zyedidia/arm64/str_z_bi.html>; LDR
  symmetric); `RDVL <Xd>, #imm` multiplies VL-in-bytes by imm, so
  `rdvl x0, #1` yields VL bytes
  (<https://www.scs.stanford.edu/~zyedidia/arm64/rdvl_r_i.html>); `CNTB`
  with default pattern also equals VL bytes
  (<https://www.scs.stanford.edu/~zyedidia/arm64/cntb_r_s.html>); index:
  <https://www.scs.stanford.edu/~zyedidia/arm64/sveindex.html>. **Caveat:**
  the immediate-scaling unit for STR/LDR (predicate) (PL vs VL) was not
  verified — this design uses zero-offset addressing only, so it never
  matters.
- **HWCAP**: `HWCAP_SVE = 1 << 22` in `AT_HWCAP`
  (<https://github.com/torvalds/linux/blob/master/arch/arm64/include/uapi/asm/hwcap.h>);
  kernel SVE ABI confirms the aux-vector report, `/proc/cpuinfo` `sve`,
  `PR_SVE_GET_VL`, and `16 <= VLmax <= 256` bytes
  (<https://docs.kernel.org/arch/arm64/sve.html>).
- **QEMU**: qemu-aarch64 (user mode) supports `sve-default-vector-length=N`
  (bytes, −1 = max, default 64; max 256 bytes = 2048-bit) mirroring
  `/proc/sys/abi/sve_default_vector_length`
  (<https://www.qemu.org/docs/master/system/arm/cpu-features.html>);
  `sve-max-vq` (range 1–16) is still registered on the TCG `max` CPU and is
  equivalent to enabling all lengths up to the max, though the documented
  interface is `sve<N>`
  (<https://gitlab.com/qemu-project/qemu/-/raw/master/target/arm/tcg/cpu64.c>).
  **Empirically confirmed on the authoring host** (x86-64 macOS + Docker
  Desktop, 2026-07-17): arm64 containers see `HWCAP_SVE` set with default
  VL=64 bytes; `docker run -e QEMU_CPU=max,sve-max-vq=N[,sve-default-vector-length=-1]`
  produced VL = 16 (vq=1), 48 (vq=3, non-power-of-two), and 256 (vq=16,
  dvl=−1) bytes. Note `sve-max-vq` alone does not raise the *default* VL past
  64 bytes — pair it with `sve-default-vector-length=-1`, as the T6 rule does.
- **Apple silicon**: Apple's xnu documentation states its SME implementation
  "only allows SVE accesses inside streaming SVE mode" and "simply makes SVE
  features inaccessible outside this mode" — no non-streaming SVE on any
  Apple CPU
  (<https://raw.githubusercontent.com/apple-oss-distributions/xnu/main/doc/arm/sme.md>);
  third-party M4 testing corroborates the SIGILL behavior
  (<https://dev.to/aratamizuki/trying-out-arms-scalable-matrix-extension-with-apple-m4-or-qemu-1cgh>).

## Out of scope

- **Language-binding wrappers for `asm_call_capture_sve`** (the ten-binding
  parity round that vec256/vec512 got). No sibling doc owns it yet; it is a
  natural follow-up once T8 signs off, and the parity gate deliberately does
  not force it (see Constraints). The documented-self-skip precedent for
  emulator parity is
  [binding-parity-plan.md](../archive/plans/binding-parity-plan.md).
- **Emulator (Unicorn) SVE parity** — the bundled Unicorn does not execute
  even AVX (`UC_ERR_INSN_INVALID`); wide-vector capture is native-only until
  upstream moves, per Track D deliverable 3 in
  [post-v1-expansion-plan.md](../plans/post-v1-expansion-plan.md).
- **A `vec512_t` manifest entry** — observed missing from
  `scripts/gen-manifest.c` while verifying T5's pattern; noted here for
  whoever owns manifest completeness, not added silently in this work.
- **AArch64 tracer/CLI work**: single-step/watchpoint support is
  [asmspy-aarch64-support.md](asmspy-aarch64-support.md); ptrace single-step
  validation on arm64 hardware is
  [aarch64-ptrace-single-step-validation.md](aarch64-ptrace-single-step-validation.md).
- **CI runners that could one day host SVE silicon**:
  [self-hosted-ci-runners.md](self-hosted-ci-runners.md).
- **Publishing/packaging of anything added here**:
  [distribution-packaging.md](distribution-packaging.md).
