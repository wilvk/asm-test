# Block-step + emulator-replay value optimization — F1 spike (increment 0) findings

**Verdict: GO (2026-07-15)** for the F1 marquee bet, on pure straight-line integer/flag
code — the increment-0 exit condition is met. On the cross-validation oracle fixtures the
block-step + Unicorn-replay value trace is **byte-identical** (down to a literal `memcmp`
of the record arrays, struct padding included) to the true single-step value trace, while
**block-step cut the in-region stop count 6.06×** (303 → 50 on a loop) — the perturbation
win F1 exists to capture. The region-granularity purity classifier and the coherence
canary both work. The one genuinely unproven claim in the whole
[live-attach data-flow follow-up plan](../plans/live-attach-dataflow-followup-plan.md#f1--block-step--emulator-replay-value-optimization-planned--marquee-spike-first)
— "the emulator and the real CPU agree on a straight-line block" — held on every step of
every fixture. The verdict is **conditional** only on the boundaries this spike also
mapped (undefined-flag bits, OS-interaction, vector/XSTATE, memory coherence), each with a
concrete follow-on below; none blocks the increment-0 go/no-go.

This is a spike: the deliverables are this doc + a self-contained probe
([examples/blockstep_value_spike.c](../../../examples/blockstep_value_spike.c)). No shared
production file was modified.

---

## The bet, and how it was tested

Direct `PTRACE_SINGLESTEP` traps on **every** instruction; that stop density is exactly
what widens the cross-thread deadlock window on a live runtime. F1's idea: drive the region
with `PTRACE_SINGLEBLOCK` (one `#DB` per **taken branch** — the library already block-steps
for control flow, [asmtest_ptrace_trace_attached_blockstep](../../../src/ptrace_backend.c#L1761)),
add a real `GETREGS` snapshot at each boundary, and **replay the straight-line block**
between boundaries through the Unicorn L0 producer ([src/dataflow_emu.c](../../../src/dataflow_emu.c))
seeded with that real register state, to reconstruct the per-instruction values. The
endpoints are always real observations; replay only fills a bounded pure interior.

**Methodology — isolate the one variable.** The two shipped producers,
[dataflow_ptrace.c](../../../src/dataflow_ptrace.c) (single-step) and
[dataflow_emu.c](../../../src/dataflow_emu.c) (Unicorn), do **not** emit byte-identical
records for memory operands *by design*: the ptrace producer inlines a memory record in
read/write-set order (`open_step`), the emulator appends it via a UC hook in execution
order (`df_on_mem`). Their **slices** match — that is the shipped oracle cross-check
([test_dataflow_ptrace.c](../../../examples/test_dataflow_ptrace.c)) — but their **bytes**
do not. So a faithful byte-identical spike must hold the record-construction code
**constant** across both paths and vary **only the value source**. The probe therefore
uses **one** capture core (`open_step`/`finalize_step` over a `struct user_regs_struct` + a
pluggable memory reader) driven two ways:

- **Path A — true single-step (ground truth):** registers from `PTRACE_GETREGS`, memory
  from `process_vm_readv`, one `PTRACE_SINGLESTEP` per instruction.
- **Path B — block-step + replay:** `PTRACE_SINGLEBLOCK` to each boundary + a real
  `GETREGS`(+memory) snapshot there; the interior reconstructed by stepping Unicorn one
  instruction at a time, Unicorn **seeded from the real boundary snapshot** and its guest
  **mapped at the real addresses** so effective addresses and values compare directly. A
  coherence **canary** compares Unicorn's computed end-of-block state to the real next
  boundary.

Byte-identical between A and B ⟺ Unicorn reproduced the real CPU's per-instruction state
across the block. That is the bet, isolated to a single degree of freedom.

**Fixtures** (both x86-64 SysV leaf routines):
- `loop_poly(n)` — a register-only accumulator loop, `sum(3·i+1)` for `i∈[0,n)`; the
  *primary* case. Every taken back-edge is **one** block-step stop but **six** single-step
  stops, so it exercises both the byte-identical claim (arithmetic + *defined* flags across
  many steps) and the stop-count reduction on one region. Uses only fully-flag-**defined**
  instructions (`add`/`cmp`) plus flag-neutral `mov`/`lea` — never `xor`-to-zero (see the
  undefined-flag boundary below).
- `mem_chain(a,b)` — the shipped oracle's straight-line load-after-store + move chain
  (`df_chain`); the *memory* case (a store, a dependent load, and the `ret` stack pop) with
  no flag-affecting instruction, isolating memory-value fidelity.

---

## Result — the real output

Built + run on the host (AMD Ryzen 9 9950X, Linux 6.17.0-35, Unicorn 2.0.1, Capstone 5.0,
gcc 13.3.0):

```
gcc -O2 -g -D_GNU_SOURCE -DASMTEST_HAVE_CAPSTONE -DSPIKE_HAVE_UNICORN \
    -Iinclude $(pkg-config --cflags capstone) \
    examples/blockstep_value_spike.c src/dataflow.c src/dataflow_operands.c \
    $(pkg-config --libs capstone) -lunicorn -o /tmp/blockstep_value_spike
/tmp/blockstep_value_spike
```

```
# F1 spike: block-step + emulator-replay value optimization (increment 0)
ok 1 - purity: loop_poly classified PURE -> block-step+replay eligible
ok 2 - purity: mem_chain classified PURE -> block-step+replay eligible
ok 3 - purity: imp_syscall classified IMPURE (syscall) -> single-step fallback
ok 4 - purity: imp_rdtsc classified IMPURE (rdtsc) -> single-step fallback
ok 5 - purity: imp_cpuid classified IMPURE (cpuid) -> single-step fallback
ok 6 - purity: imp_rdrand classified IMPURE (rdrand) -> single-step fallback
ok 7 - purity: imp_int80 classified IMPURE (int 0x80) -> single-step fallback
ok 8 - loop_poly(n=50): both paths returned 3725 (A=3725 B=3725)
ok 9 - loop_poly(n=50): neither trace truncated
ok 10 - loop_poly(n=50): block-step+replay value trace is BYTE-IDENTICAL to single-step (303 steps, 854 records)
#   raw memcmp of record arrays also identical: yes
ok 11 - loop_poly(n=50): block-step CUT the stop count 303 -> 50 (6.06x fewer in-region stops)
#   single-step in-region stops = 303; block-step in-region stops = 50; captured steps = 303
ok 12 - mem_chain(7,5): both paths returned 12 (A=12 B=12)
ok 13 - mem_chain(7,5): neither trace truncated
ok 14 - mem_chain(7,5): block-step+replay value trace is BYTE-IDENTICAL to single-step (6 steps, 16 records)
#   raw memcmp of record arrays also identical: yes
ok 15 - mem_chain(7,5): block-step CUT the stop count 6 -> 1 (6.00x fewer in-region stops)
#   single-step in-region stops = 6; block-step in-region stops = 1; captured steps = 6
ok 16 - canary: injected replay-input divergence DETECTED, capture -> truncated (rc=1 truncated=1)
1..16
# all 16 checks passed
```

Deterministic: 8/8 repeat runs identical (same pass set, same 6.06× / 6.00× ratios).

### Against the increment-0 exit criteria

| Exit criterion | Result |
|---|---|
| Block-step + replay value trace **byte-identical** to single-step on a pure method | **YES** — `loop_poly` (303 steps / 854 records) and `mem_chain` (6 / 16); even the literal `memcmp` (padding included) matches |
| **Stop count** drops proportionally to mean block length | **YES** — 303 → **50** (6.06×) on the 6-insn loop body; 6 → **1** (6.00×) on the single-block chain. The ratio tracks mean block length exactly |
| An **impure** method is detected and stepped (not replayed) | **YES** — the static purity scan flags `syscall`, `rdtsc`, `cpuid`, `rdrand`, `int 0x80` (7/7 correct incl. two pure) |
| A concurrent-memory divergence is **detected** at the next real boundary and dropped to `truncated`, never silently wrong | **YES** — the injected replay-input divergence trips the canary → `truncated` |

---

## Load-bearing gotchas (record for the full F1 landing)

1. **Unicorn `UC_ERR_FETCH_UNMAPPED` on the region-exit terminator.** The block's
   terminating `ret` (or tail jump) transfers to a real address *outside* the mapped
   region; Unicorn faults trying to fetch there even under `count=1`. Fix in the probe: map
   a one-page **landing pad** at the out-of-region boundary before replaying the exit block
   so the terminator's data effects (`pop rsp`, `[rsp]` read) execute and Unicorn halts
   cleanly at the boundary. A production replay must do the same, or stop the emulator one
   instruction short and finalize the terminator's writes from the real boundary snapshot.
   *Interior* blocks whose taken branch targets an in-region, already-mapped address (loop
   back-edges) need no pad — those replay cleanly.

2. **Absolute-address identity across two processes.** The byte-identical comparison is
   defeated by *process identity*, not replay fidelity, if the two paths fork independent
   children: ASLR + the differing caller-frame depth of the two drivers put the fixture's
   `rsp` (and every stack effective-address) at different absolute values (observed: a
   0x550 delta on the `rsp` read record). The data values all matched; only stack pointers
   diverged. The probe removes this by running **both** children on **one shared stack**
   (mmap'd once in the parent, inherited COW by both forks) via a stack-switch trampoline,
   so the absolute addresses are identical too and the assertion is **literal** rather than
   "modulo the stack base." **Implication for the real F1:** in production there is exactly
   one process (block-step drives it; replay reconstructs its interior), so this artifact
   never arises — the single-step trace is only ever an offline *oracle*, and validating it
   against replay should normalize stack-absolute values (rsp-relative) exactly as the
   shipped slice-oracle already sidesteps it by keying on locations.

3. **`GETREGS + XSTATE` is *new* capture, not free reuse.** The shipped block-step
   ([ptrace_backend.c:1829](../../../src/ptrace_backend.c#L1829)) reads only PC + the return
   register via `read_pc_ret`. The replay needs the **full** GP file (all 16 GPRs + rip +
   rflags) at each boundary to seed Unicorn; the probe adds that `GETREGS`. Vector fixtures
   will additionally need `GETFPREGS`/`NT_X86_XSTATE` at the boundary and YMM/ZMM seeding
   into Unicorn — deferred (this increment is GP + flags + ≤8-byte memory).

4. **eflags TF/RF are debug-mechanism bits — masked defensively, but proved a no-op here.**
   The probe masks `TF`(bit 8)/`RF`(bit 16) out of every captured eflags, on the theory that
   a single-stepped tracee could surface `TF` in `GETREGS` where a block-step boundary does
   not. **Empirically, on this kernel the mask was unnecessary** — the byte-identical result
   holds with the mask disabled, i.e. Linux 6.17 does not surface `TF` in `GETREGS` and the
   emulator (seeded from the real boundary eflags) reproduces the **full** architectural
   eflags including `IF`. The mask is retained as a cheap, documented normalization because
   `TF` visibility is kernel/hypervisor-dependent, not because it was needed here.

5. **Undefined-flag bits are a *theoretical* divergence surface — not observed.** Intel/AMD
   leave `AF` (and others) **architecturally undefined** after `xor`/logic ops, so an
   emulator and silicon are permitted to disagree there; the primary fixture avoids
   `xor`-to-zero for exactly this reason. A direct experiment (swap `mov eax,0` →
   `xor eax,eax`, whose `AF` is undefined, and re-run) came out **byte-identical anyway** on
   this host — Unicorn 2.0.1 and this Zen 5 silicon coincidentally agree (both yield `AF=0`
   for logic ops). This is reported honestly as a *risk*, not a demonstrated failure: a
   production replay should either mask officially-undefined flag bits at capture or restrict
   replay to instructions with fully-defined flags, since a different silicon/emulator pairing
   could diverge where this one did not.

   **LANDED 2026-07-18** ([dataflow-producer-correctness.md](../implementations/dataflow-producer-correctness.md)
   T4): an explicit mnemonic(+count)-keyed table (`dfb_undef_flags`,
   `src/dataflow_blockstep.c`) now masks every architecturally undefined EFLAGS bit out of
   both the coherence canary and the captured EFLAGS write records, on both the oracle and
   replay paths. The risk this gotcha flagged is closed structurally — mask-then-compare,
   not "this pairing happens to agree" — rather than merely re-confirmed on one more host;
   a `xor eax,eax` fixture (the case this gotcha's own experiment used) proves AF reads 0 in
   every post-xor EFLAGS record while the trace stays byte-identical, and a dedicated
   canary-discrimination test proves the mask, not luck, is what tolerates it.

6. **The purity scan is a linear sweep.** The classifier linearly disassembles the region's
   bytes once (Capstone) and flags `syscall`/`sysenter`/`int 0x80`/`rdtsc`/`rdtscp`/
   `rdrand`/`rdseed`/`cpuid` — exact for the fixtures (straight byte streams). A production
   classifier should sweep the method's real instruction extents (it has the JIT method-map)
   or follow decoded control flow, so embedded data / bytes past an indirect branch cannot
   misdecode. Classifying **per region up front** is what sidesteps the plan's ordering trap
   (block-step advances the *real* process, so a syscall inside a block has already retired
   by the boundary — never emulate through it).

   **LANDED 2026-07-18** ([dataflow-producer-correctness.md](../implementations/dataflow-producer-correctness.md)
   T7): `region_scan` now accepts an optional caller-vouched list of real instruction extents
   (`asmtest_blockstep_extent_t`, blob-absolute, sorted/non-overlapping/in-range — validated
   before any tracee is spawned) and sweeps each independently; bytes outside every extent
   are never fetched, so an embedded island between two extents cannot desync the decoder.
   The tier stays agnostic about extent provenance (a JIT method map is the intended source,
   but nothing here reads one). NULL/0 keeps the pre-T7 whole-region linear sweep exactly as
   this gotcha described it — extents are additive, not a replacement for the fail-closed
   desync behaviour the earlier fix already landed.

7. **`PTRACE_SINGLEBLOCK` must be probed functionally.** Some hypervisors mask
   `DEBUGCTL.BTF`, silently degrading block-step to per-instruction stepping. The probe
   reuses the library's hang-proof functional probe (a `#DB` at the head of a nop run must
   leave the blob in one `SINGLEBLOCK`) and self-skips the live half cleanly if it fails, or
   if ptrace is blocked (seccomp/yama) or the build lacks Unicorn/Capstone. The purity scan
   (Capstone-only) still runs in those cases.

---

## Consequences and recommended next step

- **Increment 0 is GO.** The value-reconstruction claim — the one genuinely unproven bet in
  the design — holds byte-for-byte on OS-free integer/flag code, and the perturbation win is
  real and scales with mean block length (~6× here; production JIT blocks average ~5–8
  instructions, so an order-of-magnitude-fewer-stops claim is well-founded). F2/F5, which
  build on the replay, are **unblocked**.
- **Recommended first landing step (F1 increment 1):** wire the replay into the shipped
  block-step producer as a value tier. Concretely: extend
  [asmtest_ptrace_trace_attached_blockstep](../../../src/ptrace_backend.c#L1761) to take a
  full-`GETREGS` snapshot at each boundary, and add a `dataflow_blockstep.c` producer that,
  per pure region, seeds a persistent Unicorn engine (guest mapped at the tracee's real
  addresses, memory faulted per boundary from `process_vm_readv`) and reconstructs the
  interior into the shared `asmtest_valtrace_t`, gated by the purity scan (impure → fall back
  to the existing single-step producer). Validate it against the single-step producer with a
  **location-normalized** (rsp-relative) equality, since production is one process and the
  single-step trace is only an oracle. Reuse the coherence canary (Unicorn end-of-block vs
  the real boundary) to drop a divergent region to `truncated`.
- **Then, in order:** F2 (record-and-inject for impure methods — replay through recorded
  syscall/`rdtsc` effects), vector/XSTATE seeding (YMM/ZMM), and undefined-flag masking. Each
  is a bounded extension of what this spike proved; none re-opens the go/no-go.
- The probe is a **manual diagnostic** — a self-contained host program, not wired into CI
  (it needs real ptrace + Unicorn, and it characterizes a research bet rather than gating the
  product). Rebuild/run with the two commands above.

---

## Files

- [examples/blockstep_value_spike.c](../../../examples/blockstep_value_spike.c) — the probe
  (purity scan + Path A single-step + Path B block-step/replay + byte-identical comparator +
  coherence canary + self-skip probes). Links `src/dataflow.c` + `src/dataflow_operands.c`
  for the shared L0 sink + Capstone operand enumerator; needs `-lunicorn -lcapstone`.
- This doc.
