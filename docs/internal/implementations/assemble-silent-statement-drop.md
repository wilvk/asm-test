# The in-line assembler silently drops statements it cannot parse — implementation

> **Source.** Not actioned from a plan: found 2026-07-22 while closing
> [distribution-packaging.md](distribution-packaging.md) T5's CI-gated leg —
> exercising the published manylinux wheel in a clean AlmaLinux 8 container
> surfaced it, and nothing in `docs/internal/` records it. Every measurement
> below was taken against the pinned Keystone in `asmtest-bindings-base` on
> 2026-07-22 with the probe described in *What was measured*. If the CODE and
> this doc disagree, re-verify before implementing.

## Why this work exists

`asmtest_assemble()` can return machine code that is **missing an instruction
the caller wrote**, with `ok == true` and an empty error string.

```
source:  mov rax, 1        assembled:  48 c7 c0 01 00 00 00   mov rax, 1
         mov rbx, #2                   48 c7 c1 03 00 00 00   mov rcx, 3
         mov rcx, 3                    c3                     ret
         ret
                           returned:   ok=true, err="", 3 instructions from 4 statements
```

For a library whose purpose is asserting on machine code this is the worst
failure shape available: the assertion runs, passes or fails, and reports on
code that is **not what the caller wrote**. A wrong answer is louder than a
missing one; this is a quiet wrong answer.

The most reachable trigger is not an exotic typo — it is **AT&T source
assembled with the default Intel syntax**:

```c
asmtest_assemble(ASM_X86_64, ASM_SYNTAX_INTEL, "movq $42, %rax\nret\n", 0, &r);
/* -> ok=true, len=1, bytes={0xc3}.  The mov is gone. */
```

`ASM_SYNTAX_INTEL` is the default every binding passes when the caller does not
name a syntax (`assemble(src)` in Python, and the equivalent in the other nine),
so a user who pastes AT&T text gets a silent one-byte `ret` rather than a
diagnostic. The same shape swallows an ARM-style immediate on x86
(`mov rax, #42`) and a truncated operand (`mov rax,`).

This is a **core defect, not a binding defect**: all ten language bindings reach
the assembler through the one `asmtest_asm_bytes` FFI
([src/assemble.c:274](../../../src/assemble.c#L274)), which forwards to
`asmtest_assemble` ([src/assemble.c:102](../../../src/assemble.c#L102)). Fixing
the core fixes all ten at once, and no binding can work around it today —
`asmtest_asm_bytes` returns only a byte count, never `stat_count`.

## What already exists (verified 2026-07-22)

- **The one choke point.** `asmtest_assemble`
  ([src/assemble.c:102-153](../../../src/assemble.c#L102)) is the only caller of
  `ks_asm` in the tree. Every other entry point funnels through it: the FFI
  shim `asmtest_asm_bytes` ([:274](../../../src/assemble.c#L274)), the emu
  bridges via `assemble_at_base` ([:175](../../../src/assemble.c#L175)) —
  `emu_call_asm`, `emu_arm64_call_asm`, `emu_riscv_call_asm`, `emu_arm_call_asm`
  — and `asmtest_emu_call_asm6` ([:244](../../../src/assemble.c#L244)). **A guard
  added inside `asmtest_assemble` covers the entire surface**; no other file
  needs to change.
- **The count is already captured and already public.** `asmtest_assemble`
  stores Keystone's statement count at
  [src/assemble.c:147](../../../src/assemble.c#L147) into `asm_result_t.stat_count`,
  documented as "assembly statements successfully processed"
  ([asmtest_assemble.h:65](../../../include/asmtest_assemble.h#L65)). The value
  the fix needs is therefore **already in hand** — it is simply never checked.
- **The existing error path is correct** and is the model to follow:
  [src/assemble.c:125-134](../../../src/assemble.c#L125) formats
  `ks_strerror(ks_errno(ks))` plus "(after N statements)", sets the thread-local
  last-error, and returns false. The new guard should produce the same shape so
  bindings need no new error plumbing.
- **The syntax rules the guard needs are already known in-tree.** `test_asm.c`'s
  `x86_nasm_masm_gas_match_intel` case carries the comment "NASM treats ';' as a
  comment, so its statements are newline-separated; MASM (Intel-family) and GAS
  (AT&T-family) accept ';' as before"
  ([examples/test_asm.c:48-50](../../../examples/test_asm.c#L48)) — and the
  header documents statements as "separated by ';' or newline"
  ([asmtest_assemble.h:71](../../../include/asmtest_assemble.h#L71)).
- **The test suite to extend** is
  [examples/test_asm.c](../../../examples/test_asm.c) (235 lines, `make asm-test`,
  built at [Makefile:903](../../../Makefile#L903)). It already asserts on
  `stat_count` directly — `ASSERT_EQ(r.stat_count, 3)`
  ([:28](../../../examples/test_asm.c#L28)) — so the field is established test
  vocabulary, not something this doc introduces.
- **The corpus a false-positive sweep must clear**: ~76 call sites across
  `examples/`, `bindings/`, `tools/`, `tests/`. None of the C ones currently
  passes a source containing a `;` inside a string literal or comment, which is
  the shape most likely to trip a naive counter (see T2's risk list).

## What was measured

Two probes against the pinned Keystone in `asmtest-bindings-base` (a ~20-line C
file calling `ks_asm` directly and printing `rc`, `ks_errno`, `stat_count`,
and the encoding). Re-run them before implementing — the numbers below are the
spec's foundation, and a Keystone bump could move them.

**1. The failure is invisible to every signal the current code checks.**

| source (x86-64, Intel) | `ks_asm` rc | `ks_errno` | `stat_count` | bytes |
|---|---|---|---|---|
| `mov rax, 1 / mov rbx, 2 / mov rcx, 3 / ret` | 0 | `KS_ERR_OK` | 4 | 4 insns |
| `mov rax, 1 / mov rbx, #2 / mov rcx, 3 / ret` | **0** | **`KS_ERR_OK`** | **3** | **3 insns** |
| `mov rax, 1 / foo bar / ret` | -1 | `MNEMONICFAIL` | 0 | — |

The middle row is the defect. `rc` is success and `ks_errno` is `OK` — **so
checking `ks_errno` does not fix this**, and `stat_count` is the only witness.
Note the third row: a bad *mnemonic* fails loudly. What gets dropped silently is
a statement that parses as far as a recoverable error — bad operand, truncated
operand, wrong-dialect operand.

**2. It reproduces in all five x86 syntaxes**, so the guard must not be
syntax-conditional. Same 4-statement source, one bad statement:

| syntax | rc | `stat_count` | instructions emitted |
|---|---|---|---|
| INTEL | 0 | 3 | 3 of 4 |
| NASM | 0 | 3 | 3 of 4 |
| MASM | 0 | 3 | 3 of 4 |
| ATT | 0 | 3 | 3 of 4 |
| GAS | 0 | 3 | 3 of 4 |

**3. `stat_count` is a *lower bound*, never an over-strict one — and `;` is
syntax-dependent.** This is what makes the guard safe in one direction and
dangerous in the other:

| source | INTEL | NASM | MASM | ATT | GAS |
|---|---|---|---|---|---|
| `mov rax, 1; mov rbx, 2` + `ret` | 3 | **2** | 3 | 3 | 3 |
| `mov rax, 1 ; hello` + `ret` | rc=-1 | **2** | rc=-1 | rc=-1 | rc=-1 |
| `mov rax, 1` + `# hi` + `ret` | **4** | 4 | 4 | 4 | 4 |
| `l1:` + `.align 4` + `mov rax, 1` + `jmp l1` | 4 | rc=-1 | 4 | 4 | 4 |

Read off the two rules the guard depends on:

- **`;` separates statements in INTEL/MASM/ATT/GAS and starts a comment in
  NASM.** A source-side counter that splits on `;` unconditionally will
  over-count NASM sources and **reject valid code**. This is the single largest
  false-positive risk in the fix.
- **Keystone over-counts relative to instructions** — a comment-only line and a
  label each add to `stat_count` (row 3: 4 for two instructions). Over-counting
  is the *safe* direction: a `stat_count < expected` test can only fire when
  Keystone genuinely processed fewer statements than the source contains.

## Tasks

### T1 — A regression test that fails on today's code  (S, depends on: none)

**Goal.** The defect is pinned by a test before any fix exists, so the fix is
proven to be what closes it.

**Steps.**
1. Add to [examples/test_asm.c](../../../examples/test_asm.c), beside the
   existing assembler-core cases, a `TEST(assemble, rejects_dropped_statement)`
   asserting that `asmtest_assemble(ASM_X86_64, ASM_SYNTAX_INTEL,
   "mov rax, 1\nmov rbx, #2\nmov rcx, 3\nret\n", EMU_CODE_BASE, &r)` returns
   **false**, and that `r.err` is non-empty.
2. Add `TEST(assemble, rejects_att_source_in_intel_syntax)` for the reachable
   trigger: `"movq $42, %rax\nret\n"` under `ASM_SYNTAX_INTEL` must return false,
   **not** `{0xc3}`. Name the real-world shape in the comment — this is the case
   a binding user hits by pasting AT&T text.
3. Keep both cases in the "Assembler core: deterministic, host-independent"
   section — they run on any host, no emulator, no silicon.

**Code.** `examples/test_asm.c` only.
**Tests.** `make asm-test` — the two new cases **fail** at this commit. That is
the point of T1; land it with the fix in T2 rather than pushing a red suite (see
*Task order*).
**Docs.** None.
**Done when.**
- Both cases fail against unmodified `src/assemble.c`, each for the documented
  reason (returns true / returns the short encoding), verified by running them.

### T2 — Guard `asmtest_assemble` on the statement count  (M, depends on: T1)

**Goal.** A source whose statements were not all assembled is a failure, with a
diagnostic naming what happened.

**Steps.**
1. Add a `static size_t count_statements(const char *src, asm_syntax_t syntax)`
   to [src/assemble.c](../../../src/assemble.c) returning a **lower bound** on
   the statements Keystone should report. Walk the source once:
   - split on `\n`, and **also on `;` unless `syntax == ASM_SYNTAX_NASM`** (the
     measured rule — see *What was measured* #3);
   - within a chunk, truncate at the first comment introducer: `#` for every
     syntax, plus `;` for NASM (this is the same `;` rule from the other side);
   - skip chunks that are empty or whitespace-only after truncation;
   - **do not** try to classify labels or directives — they only ever make
     Keystone's count larger, and the comparison is `<`.
2. In `asmtest_assemble`'s success path
   ([src/assemble.c:137-152](../../../src/assemble.c#L137)), after `ks_asm`
   returns 0, compare `stat_count` against `count_statements(source, syntax)`.
   If `stat_count < expected`, free the encoding and fail through the existing
   `fail()`/`set_last_err` path with a message naming the count —
   e.g. `"assembler skipped %zu of %zu statements (check the syntax argument)"`.
   Mention the syntax argument explicitly: the dominant cause is a dialect
   mismatch, and that is the caller's next question.
3. Do **not** change `asmtest_asm_bytes`, the emu bridges, or any binding — they
   inherit the guard through `asmtest_assemble` and already surface
   `asmtest_asm_last_error()`.
4. **Prove the guard cannot reject valid code.** Add a temporary harness (or a
   scratch `main`) that runs `count_statements` against every assembler source
   in the tree and asserts `stat_count >= expected` for each: the ~76 call sites
   under `examples/`, `bindings/`, `tools/`, `tests/`, plus every case already in
   `test_asm.c`. **A single false rejection here blocks the fix** — a guard that
   refuses valid input is worse than the bug it closes.
5. Exercise the constructs most likely to break the counter and record the
   result in the doc-tail (T3), whether they pass or need handling: a `;` or `#`
   inside a string literal (`.ascii "a;b"`), a label sharing a line with an
   instruction, a multi-statement NASM line, and each of the five syntaxes'
   comment forms.

**Code.** `src/assemble.c` only.
**Tests.** `make asm-test` (T1's two cases now pass, all pre-existing cases
unchanged); `make test`; `make check`; `make docker-bindings` — the ten wrappers
all assemble through this path, so a false rejection would surface there.
**Docs.** Changelog `Fixed`.
**Done when.**
- T1's two cases pass and every pre-existing `test_asm.c` case is unchanged.
- The step-4 sweep reports zero false rejections across the whole in-tree corpus,
  and the run is recorded.
- `make docker-bindings` is green.

### T3 — Document the contract and the residue  (S, depends on: T2)

**Goal.** The guarantee is stated where a caller looks, and the part of the
problem the guard does *not* solve is named rather than implied closed.

**Steps.**
1. State the contract on `asmtest_assemble` in
   [include/asmtest_assemble.h](../../../include/asmtest_assemble.h): every
   statement in the source is assembled or the call fails — no partial output.
   Note that `stat_count` may exceed the instruction count (comments, labels,
   directives), so it is a statement counter, not an instruction counter.
2. Record the residue honestly in the doc-tail: the guard catches a **dropped**
   statement. It does not catch a statement that assembles to something the
   caller did not intend (a real dialect ambiguity), and it rests on a
   Keystone behaviour measured at one pinned version — a Keystone bump should
   re-run the *What was measured* probes.
3. Note the user-facing symptom in the assembler's user documentation: if
   `assemble` reports skipped statements, the first thing to check is the
   `syntax` argument.
4. Changelog entry under `Fixed` naming the silent-wrong-output shape, since
   callers may have passing tests today that assert on short code.

**Code.** None.
**Tests.** `make docker-docs` (Sphinx `-W`).
**Done when.**
- The header states the all-or-nothing contract; the residue and the Keystone
  version-sensitivity are recorded; `make docker-docs` is clean.

## Task order & parallelism

Strictly sequential — T1 → T2 → T3 — and all three land in **one commit**. T1's
tests are red by construction, so pushing them alone would leave `main` failing
`make asm-test`; author them first (so the fix is demonstrably what turns them
green), then land the pair together. There is no parallelism worth taking here:
the whole change is two functions in one file.

**Soft coordination:** none. `src/assemble.c` is not touched by any other open
doc in this directory.

## Constraints & gates

- **No hardware or credential gate.** Keystone runs everywhere the test suite
  does; this doc is fully implementable and fully verifiable on any x86-64 Linux
  host with the standard Docker lanes. It must not self-skip anywhere.
- **False rejections are the failure mode that matters.** The bug being fixed
  makes a wrong answer quiet; a bad fix makes valid input fail loudly, which is
  a worse regression for every downstream test suite. T2 step 4 is not optional
  and is not a formality — treat a single false rejection as a blocker.
- **Keystone is pinned** ([scripts/build-keystone.sh](../../../scripts/build-keystone.sh),
  0.9.2). The measured statement-counting rules are properties of that version;
  do not carry them forward through a bump without re-running the probes.
- **Do not "fix" this by splitting the source and assembling per statement.**
  It breaks labels, relative branches, and `.align` — every construct whose
  encoding depends on the statements around it.

## Out of scope

- Changing the default syntax, or auto-detecting the dialect from the source.
  The fix is to *report* the mismatch, not to guess around it.
- Widening `asmtest_asm_bytes`'s FFI signature to return `stat_count`. The guard
  belongs in the core, where it covers all ten bindings without an ABI change.
- Keystone's own error recovery. Upstream may consider partial assembly with a
  success return intentional; this doc makes our wrapper's contract explicit
  regardless of what upstream decides.

## Implementation record — 2026-07-23 (T1-T3 landed)

Implemented on the Zen 5 dev box against the pinned Keystone 0.9.2 in
`asmtest-ci` (`ks_version` reports 0.9). Everything below was re-measured before
implementing, as the doc's own preamble requires.

### Corrections to *What was measured*

The doc's tables were re-run and hold, with **two corrections**:

1. **Table 2 overstates the syntax spread for its own example.** With the *same
   Intel-syntax* 4-statement source, `ATT` and `GAS` do **not** silently drop —
   they fail loudly (`rc=-1`, `KS_ERR_ASM_MNEMONICFAIL`), because Intel `mov
   rax, 1` is not AT&T at all. Written in each dialect's own syntax, the bad-`#`
   operand also fails loudly in ATT/GAS (`KS_ERR_ASM_INVALIDOPERAND`). The
   *conclusion* the table draws is nonetheless correct and is what the guard
   rests on: a **truncated** operand (`mov rax,` + `ret`) drops silently in all
   five x86 dialects — `rc=0`, `stat_count=1`, one byte out — so the guard must
   not be syntax-conditional.
2. **The defect is not x86-only.** It reproduces on the ARM guests the emu
   bridges assemble: `mov x0, #1 / mov x1, $2 / ret` on ARM64 gives `rc=0`,
   `stat_count=2` of 3; `mov x0,` + `ret` gives 1 of 2. Same on ARM32. The doc
   scoped the fix to x86 syntaxes; the guard is arch-wide, and T1 gained a case
   for it.

### The `#` rule the doc got wrong, and why it matters

T2 step 1 prescribes truncating each chunk at `#` "for every syntax". That is
**wrong for ARM and would have cost most of the detection there**: `#` is the
immediate prefix, so `mov x0, #1; mov x1, #2; ret` (measured: `stat_count=3`)
would have been counted as **one** statement, and any drop after the first `#`
made invisible. It is also wrong for x86/NASM, where `#` is not a comment
(`mov rax, 1 # a;b` fails with `KS_ERR_ASM_INVALIDOPERAND`) — harmless, since
that never reaches the success path, but not a rule worth stating falsely. The
shipped `stmt_rules()` therefore measures out three knobs rather than one:

| dialect | `;` | `#` | `//`, `/* */` |
|---|---|---|---|
| x86 Intel / MASM / AT&T / GAS | separator | comment | comment |
| x86 NASM | **comment** | not a comment (parse error) | comment |
| ARM64 / ARM32 | separator | **immediate prefix** | comment |

### T2 step 5 — the constructs that break a naive counter

Each was measured, and each **needed handling**: a naive splitter over-counts
and rejects valid code in all four cases. `want` is what the shipped counter
returns; `stat` is Keystone's.

| construct | naive split | shipped `want` | `stat` | verdict |
|---|---|---|---|---|
| `.ascii "a;b"` + `ret` | 3 | 2 | 2 | needed string tracking |
| `.byte ';'` + `ret` | 3 | 2 | 2 | needed **char**-literal tracking |
| `mov rax, 1 // a;b` + `ret` | 3 | 2 | 2 | needed `//` handling |
| `mov rax, 1 /* a;b */` + `ret` | 3 | 2 | 2 | needed block-comment handling |
| `/* a;` … `b */` across a newline | 3 | 2 | 2 | block comment spans lines |
| `.ascii "a\";b"` | 3 | 2 | 2 | needed `\"` escape handling |
| `mov rax, 1 # a;b` + `ret` | 3 | 2 | 2 | `#` truncation covers it |
| NASM `mov rax, 1; mov rbx, 2; ret` | 3 | 1 | 1 | the doc's headline NASM rule |
| `mov rax, 1;;ret` | 2 | 2 | 3 | empty chunk skipped, safe |
| label sharing a line, `l1: mov rax, 1; jmp l1` | 3 | 3 | 3 | exact |
| CRLF + blank lines | — | 2 | 8 | Keystone over-counts, safe |

`'` is tracked as a literal delimiter even though an **unterminated** one (GAS
`'a`) then swallows the rest of the source. That is deliberate: swallowing only
*lowers* the count, costing a detection rather than rejecting valid code, which
is the direction the doc mandates.

### T2 step 4 — the false-rejection sweep

A scratch harness (`#include "../src/assemble.c"`, not committed) split the
question in two, because they are not the same question:

- **NATIVE** — every source in the dialect it is actually written for. Any
  `want > stat` here is a false rejection and blocks the fix.
- **CROSS** — the same sources under every other `(arch, syntax)`. `want > stat`
  here is the guard *working*: a dialect mismatch is the silent drop being
  closed.

Corpora: the 27 string literals extracted from the ~103 assemble call sites
under `examples/`, `bindings/`, `tools/`, `tests/`, `src/`, tagged with the
dialect each call site passes; the adversarial shapes above; and the
`examples/*.s` / `*.asm` corpus files whole. The raw `.s` files are C-preprocessor
sources (`#include "asm.h"`, `#if defined(__x86_64__)`) that Keystone cannot
parse, so they were also swept **after the preprocessing the build applies**
(`cpp -C`, comments kept, unsupported directives dropped) — that form is what a
user actually pastes into `assemble()`, and it is where the large-source stress
lives: 14 files assembling cleanly at 9-74 counted statements against Keystone's
36-131.

Result, re-run at the final commit:

```
# NATIVE: 47 tagged sources + 42 *.s + 19 *.asm corpus files
# NATIVE: 74 assembled cleanly and passed the guard, 95 skipped (ks_asm failed loudly / unsupported guest)
# NATIVE: 31 of those sat at the guard's tightest (want == stat)
# CROSS:  123 clean assembles in a foreign dialect, 21 of them caught as dropped statements
# keystone hangs (excluded, pre-existing): 19
# anti-vacuity: 8 fixtures caught, 0 missed
# whole-file drops caught (unsupported ISA, KS_ERR_OK + 0 bytes): 1
# FALSE REJECTIONS: 0
```

The 31 exact-count cases matter more than the total: those are the sources where
the guard sits at its tightest bound and a single over-count would have fired.

### Two findings the sweep surfaced

- **Keystone drops a whole unsupported-ISA source with `KS_ERR_OK` and zero
  bytes.** `examples/apx_basic.s` is Intel APX (EGPR + NDD). Keystone 0.9.2 has
  no APX support and does not say so: `addq %rsi, %rdi, %rdx` returns `rc=0`,
  `ks_errno=KS_ERR_OK`, `stat_count=0`, **0 bytes**. Before this change,
  `asmtest_assemble` on that file returned `ok=true` with an empty buffer — the
  defect in its most extreme form. The guard catches it (`want=26 stat=0`). It
  is *not* pinned by a test, deliberately: a Keystone bump that adds APX would
  legitimately flip the behaviour, and the doc's rule is to re-measure across a
  bump rather than freeze a version's quirk.
- **Keystone hangs on a large NASM-syntax source** (>8s, all 19
  `examples/*.asm` files under `ASM_SYNTAX_NASM`). Pre-existing, inside
  `ks_asm` and reached before any code here runs — the guard neither causes nor
  fixes it. Recorded because a binding user assembling a large NASM source will
  hit it; out of scope for this doc.

### Residue — what this does not close

- **The guard catches a *dropped* statement, not a *misassembled* one.** A
  statement that parses in the requested dialect but means something the caller
  did not intend still assembles silently. `mov rax, rdi` is a valid, and
  different, instruction under Intel and under a dialect where the operand order
  flips; nothing here can see that.
- **It rests on Keystone behaviour measured at one pinned version** (0.9.2, via
  `scripts/build-keystone.sh`). The three dialect rules in `stmt_rules()`, the
  over-count direction of `stat_count`, and the APX finding are all properties
  of that build. **Re-run the *What was measured* probes across a Keystone
  bump** — the probe programs are ~20 lines of `ks_asm` printing `rc`,
  `ks_errno`, `stat_count` and the encoding.
- **The counter is a lower bound by construction, so some drops go unseen.** An
  unterminated `"` or `'`, or a drop of a statement that the counter had already
  folded into a comment, will not be detected. This is the deliberate trade: a
  missed detection is a return to today's behaviour for that source, whereas an
  over-count would reject valid input for every downstream suite.
