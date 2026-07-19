# Testing future-ISA assembly with Intel SDE

Some assembly uses an instruction-set extension the machine running the tests
does not have: Intel **APX**'s extended registers `r16`–`r31`, **AVX10.2**,
**AMX**, or plain **AVX-512 on an AVX2-only box**. Those routines are normally
untestable here — the [DynamoRIO native tier](native-tracing.md) executes on the
real CPU (so an unsupported instruction faults), and the
[Unicorn emulator tier](traces.md) vendors a QEMU whose TCG predates AVX.

Intel's **Software Development Emulator (SDE)** closes that gap. SDE emulates
future and absent ISA extensions for the **whole process**, so running an
**unmodified** suite binary under `sde64 -future` gives future-ISA routines the
same full register / flag / memory / ABI assertion battery every other test gets
— on **any x86-64 host**, including CI runners with no special silicon.

| Backend | Records | Runs where |
|---|---|---|
| Intel SDE emulation under `sde64 -future` | full register / flag / memory / ABI assertions on emulated ISA | any x86-64 Linux |

---

## Running the lane

The pinned, digest-verified SDE kit and the APX-capable assemblers live in a
container image, so the simplest way to run the lane is:

```sh
make docker-sde
```

That builds `Dockerfile.sde` (Intel SDE 10.8.0 + GNU binutils 2.46.1 for an
APX-capable GAS + NASM 3.02 + libunicorn, each SHA-256-pinned) and runs the suite
under `sde64 -future`.

To run on a host directly, fetch the pinned kit and point the lane at it:

```sh
make sde-test SDE_HOME=$(scripts/fetch-sde.sh)
```

`scripts/fetch-sde.sh` downloads SDE 10.8.0, refuses it unless its SHA-256 matches
the pinned digest in `scripts/third-party-digests.txt`, and prints `SDE_HOME`. The
lane self-skips with a printed reason on a non-x86-64 host (SDE is x86-only) and
when `SDE_HOME` is unset (it points you at the fetch script).

---

## What the lane proves

**Transparency (the null test).** SDE must not change the behaviour of correct
baseline code. `sde-test` runs suites with no capability-dependent skips
(`test_arith`, `test_capture`, `test_mem`) both natively and under
`sde64 -future` and asserts the two TAP streams are **byte-for-byte identical**.
Any difference *is* a transparency-violation report.

**The APX example.** `examples/apx_basic.s` uses the extended registers `r16`–`r19`
(via REX2 encodings) and the new-data-destination (NDD) 3-operand form.
`examples/test_apx_basic.c` gates every case on a CPUID probe, so on real pre-APX
silicon it reports `# SKIP APX not available` (it can never rot into a vacuous
pass), while under `sde64 -future` — whose emulated CPUID reports `APX_F` — the
gate opens and the routines actually execute and are asserted. This is the lane's
reason to exist: those routines run **nowhere else** in the test suite.

**The AVX-512-on-AVX2 un-skip.** The existing `test_simd` suite skips its AVX-512
case on a host without AVX-512F. Under `sde64 -future` that case runs as a real
execution — the lane converts a documented capability self-skip into an assertion,
regardless of the host's true AVX support.

**The instruction-mix report.** `make sde-mix` histograms a suite's dynamic
instructions with SDE's emulator-aware `-mix` tool and folds the process-wide
total into the repo's canonical `asmtest_trace_t` report shape, then prints the
per-ISA-set breakdown that nothing else in the tree can produce.

---

## The honest boundary

SDE is an **emulator**, so what the lane asserts holds *up to emulation fidelity*.
Timing and the exact silicon-level ABI are not what SDE guarantees — it guarantees
architectural results (registers, flags, memory, control flow). Two anchors keep
that fidelity honest:

- **Native TAP identity** (the transparency null test above) proves SDE reproduces
  the real machine byte-for-byte on ISA the host *does* support.
- **The Unicorn cross-check** (`sde-crosscheck-test`) runs the same baseline
  routines natively-under-SDE **and** through the independent Unicorn emulator
  tier and asserts they agree — an emulator-versus-emulator anchor on the ISA both
  model.

For the full design — the pinning discipline, the license posture (SDE is
test-lane-only and never bundled), and the per-task validation — see the
[implementation note](https://github.com/wilvk/asm-test/blob/main/docs/internal/implementations/pin-sde-future-isa-lane.md).
