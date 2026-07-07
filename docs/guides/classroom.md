# Teaching with asm-test

asm-test was built for exactly the failure modes student assembly produces:
a segfault is a reported test failure (not a dead grader), an infinite loop is
a timeout, a clobbered callee-saved register is an `ASSERT_ABI_PRESERVED`
diagnostic, and the emulator tier runs AArch64/ARM32/RISC-V student code on
whatever laptop or runner you have. This page is the instructor recipe: how to
structure an assignment, autograde it with GitHub Classroom, and grade the
things that matter in an assembly course.

> The one missing piece is a maintained `asm-test-assignment-template`
> repository you can click "Use this template" on — until it exists, the three
> files below *are* the template; they are complete and copy-paste ready.

## Why the primitives fit grading

- **Fork isolation + timeout** (default): one student's `SIGSEGV` or `while(1)`
  cannot take down the grading run — it becomes one failed test with a named
  signal, and the rest of the rubric still executes.
- **JUnit output** (`--format=junit`): GitHub Classroom, GitLab, Moodle
  CodeRunner-style wrappers, and every CI dashboard ingest it natively.
- **Per-test scoring**: `--filter` runs one rubric item at a time, so each
  autograding step can carry its own points.
- **ABI grading**: `ASSERT_ABI_PRESERVED` / `ASSERT_FLAG_SET` /
  `ASSERT_REG_EQ` grade the *convention*, not just the return value — the
  distinction most courses actually teach.
- **Differential rubric items**: `ASSERT_MATCHES_REF1(student_fn, model, gen,
  10000)` grades against a C model over thousands of inputs (with failing
  inputs shrunk to readable boundary values), so hard-coding the visible test
  vectors earns nothing.
- **Cross-ISA without hardware**: the [emulator tier](emulator.md) runs
  RISC-V/ARM32/AArch64 guest routines on the x86-64 GitHub runner.

## The assignment layout

Three files per assignment; students only ever edit the first.

**`add.s`** — the student file, with the contract in comments:

```asm
/* Assignment 1: long add_signed(long a, long b)
 * Return a + b. Preserve the callee-saved registers. */
#include "asm.h"

ASM_FUNC add_signed
#if defined(__x86_64__)
    /* your code here */
    ret
#endif
ASM_ENDFUNC add_signed
```

**`grade.c`** — the rubric, one `TEST` per line item:

```c
#include "asmtest.h"

extern long add_signed(long, long);

static long ref_add(long a, long b) { return a + b; }
static int gen_pair(asmtest_rng_t *rng, long *a, int cap) {
    (void)cap;
    a[0] = asmtest_rng_long(rng);
    a[1] = asmtest_rng_long(rng);
    return 2;
}

TEST(rubric, returns_sum) {          /* 2 pts: the visible case */
    regs_t r;
    ASM_CALL2(&r, add_signed, 20, 22);
    ASSERT_EQ(r.ret, 42);
}
TEST(rubric, matches_model) {        /* 4 pts: no hard-coding */
    ASSERT_MATCHES_REF2(add_signed, ref_add, gen_pair, 10000);
}
TEST(rubric, preserves_abi) {        /* 2 pts: the convention */
    regs_t r;
    ASM_CALL2(&r, add_signed, 1, 2);
    ASSERT_ABI_PRESERVED(&r);
}
```

**`Makefile`** — installs asm-test at grade time (or vendor it as a
submodule) and builds the grader:

```make
PREFIX ?= $(HOME)/.asmtest
PC := PKG_CONFIG_PATH=$(PREFIX)/lib/pkgconfig pkg-config asmtest

grade: grader
	./grader --fail-if-no-tests

grader: add.s grade.c
	$(CC) $$( $(PC) --cflags ) -o $@ add.s grade.c $$( $(PC) --libs )

report.xml: grader
	./grader --format=junit > $@ || true
```

## GitHub Classroom autograding

Classroom's autograder is just Actions. Use the
[setup action](ci-integration.md) and one run step per scored rubric item —
`--filter` selects the item, the exit code carries pass/fail, and Classroom
attaches the points:

```json
{
  "tests": [
    {
      "name": "returns the sum",
      "run": "./grader --filter=rubric.returns_sum --fail-if-no-tests",
      "points": 2
    },
    {
      "name": "matches the reference model on 10k inputs",
      "run": "./grader --filter=rubric.matches_model --fail-if-no-tests",
      "points": 4
    },
    {
      "name": "preserves the calling convention",
      "run": "./grader --filter=rubric.preserves_abi --fail-if-no-tests",
      "points": 2
    }
  ]
}
```

with a setup workflow the steps share:

```yaml
- uses: wilvk/asm-test@main        # installs + wires pkg-config
- run: make grader
```

`--fail-if-no-tests` matters in a grader: a student who renames or deletes a
rubric test would otherwise run *nothing* and exit 0 — with the flag, an empty
selection is a scored zero, not a free pass.

## Instructor notes

- **Hidden tests**: keep a second `grade_hidden.c` in the instructor repo and
  inject it at grade time (Classroom supports post-deadline workflows); the
  differential rubric item already resists vector-memorization, so hidden
  tests are for *rubric* secrecy, not input secrecy.
- **Timeouts**: the default per-test timeout is 10 s; tighten with
  `--timeout=2` in the run steps so a spin loop costs seconds, not minutes.
- **Non-x86 courses**: build the grader against the
  [emulator tier](emulator.md) (`emu_call` / `emu_arm64_call` /
  `emu_riscv_call`) — student RISC-V runs on the stock Ubuntu runner, and
  faults surface as data (`ASSERT_FAULT_AT`), which makes good rubric items
  ("your routine must not read past the buffer").
- **Show your students the failure output**: `make demo-fail` and
  `make demo-robust` in this repo are five-minute classroom demos of what a
  diagnostic-rich failure and a contained crash look like.
