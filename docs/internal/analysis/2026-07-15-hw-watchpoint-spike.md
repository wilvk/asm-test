# F3 hardware data-watchpoint spike — findings

**Verdict: GO (2026-07-15).** x86 hardware **data watchpoints** — the debug
registers driven in *write* / *read-write* mode rather than the *execute* mode
the repo ships today — answer the data-flow question nothing else in asmspy can:
**"who touched this field, and with what value"**, at **near-zero perturbation**.
A self-contained probe ([`examples/watchpoint_spike.c`](../../../examples/watchpoint_spike.c))
armed a write watchpoint on a chosen 8-byte location in a forked victim,
`PTRACE_CONT`-ed it, and captured **every** store's value (via `process_vm_readv`)
and faulting PC (via `PTRACE_GETREGS`) — matching an independent ground-truth
sequence exactly, while the victim ran at **native speed between hits**
(measured overhead within ±7% of an unwatched baseline — i.e. noise). This is
the plan's "cheapest high-value item", and the spike confirms it: it needs no
emulator, no single-step, and no code modification, and it is a **one-field DR7
change** (the R/W + LEN fields) on top of the execution-breakpoint path already
in [`src/ptrace_backend.c:485-493`](../../../src/ptrace_backend.c#L485).

Reproducible: 4/4 runs on this host (Ubuntu 24.04, x86-64 bare metal, gcc 13.3.0,
`ptrace_scope=1`) — `values MATCH`, `stops=16`, all PCs identical, `R/W=11` also
trapped loads, exit 0 every time.

---

## What was probed

`examples/watchpoint_spike.c` — a throwaway probe (compiled directly with `gcc`,
**not** wired into the library or Makefile, **no shared file edited**). It
re-derives the DATA-watchpoint DR7 encoding independently; `ptrace_backend.c` was
read only for the `DR_OFFSET` / `PTRACE_POKEUSER` pattern. Four phases:

- **A — capture (the GO/NO-GO):** fork a victim that stores a deterministic
  `uint64_t` recurrence into a watched global, doing 2,000,000 iterations of a
  register-only mul-chain *between* each store (native-speed work that never
  touches the watched field). Arm a **write** watchpoint (`R/W0=01`, `LEN0=8B`)
  on `&g_watched`, `PTRACE_CONT`, and at each `#DB` read the post-store value +
  RIP. Assert the captured value sequence equals the tracer's independent
  recompute of the same recurrence, and that every hit's PC is identical.
- **B — perturbation:** time the watched run against an *identical* victim with
  **no** watchpoint armed. The delta is only the cost of the handful of traps.
- **C — single-step contrast:** measure this host's real per-`PTRACE_SINGLESTEP`
  cost and project single-stepping the *same* workload.
- **D — read vs write:** run the read-then-write victim once write-only
  (`R/W0=01`) and once read+write (`R/W0=11`), showing `11` additionally traps
  **loads**.

Self-skip paths: non-x86-64 (prints the AArch64 `NT_ARM_HW_WATCH` design note),
`PTRACE_POKEUSER` refused (permission / seccomp), or armed-but-never-tripped
(qemu-user emulates zero debug slots) — all exit 0 with a `# SKIP` line.

---

## Result — the captured writes (Phase A, real output)

Every store was observed with its exact value and a PC that resolves to the one
store instruction in the loop (`victim_write_sequence+0x43`, identical across all
16 hits — a loop body executes the same store address each iteration). `DR6=0x1`
confirms the B0 status bit, i.e. slot DR0 fired.

```
# Phase A — WRITE watchpoint (R/W0=01, LEN0=8B) DR7=0x90001
  hit  0: value=0x93397feca3f94613  PC=0x5b9753d78b13 (victim_write_sequence+0x43)  DR6=0x1  == expected
  hit  1: value=0x51c4cb55dfa3cb53  PC=0x5b9753d78b13 (victim_write_sequence+0x43)  DR6=0x1  == expected
  ...
  hit 15: value=0xe0764ec20ed594d3  PC=0x5b9753d78b13 (victim_write_sequence+0x43)  DR6=0x1  == expected
  stops=16 expected=16  values MATCH  all-PCs-identical=yes (same store insn)
```

The value read at the stop is the **post-store** value (the byte(s) already
written) — read out of the *tracee's* address space with `process_vm_readv`, the
same debugger-style foreign read the backend uses
([`ptrace_backend.c:315`](../../../src/ptrace_backend.c#L315)). This is exactly
"with what value"; the resolved PC is "who touched it".

## Perturbation — native speed between hits (Phases B + C, real output)

The watched victim executed **2,000,000 loop iterations between each pair of
stores** with **zero** intervening traps — only the 16 real writes stopped it.
Measured against an identical unwatched run, the overhead is indistinguishable
from noise (four runs: **-4.83%, -3.87%, +7.15%, +0.13%**):

```
# Phase B — perturbation (identical workload)
  unwatched baseline:      23.69 ms
  watched run:             22.77 ms  (16 traps)
  overhead: -3.87%  => victim ran at native speed between hits
```

The contrast with single-step — the perturbation the base live-attach tier pays
today — is stark. Per-step cost was **measured** on this host (~2.9-3.2 µs/step
over 240,040 real steps); the projection to the same workload is a labelled
lower bound (1 instruction/iteration; the real body is ~4×):

```
# Phase C — single-step contrast (per-step cost measured here)
  measured single-step: 2.946 us/step over 240040 steps
  watchpoint: 16 stops for the whole workload (22.77 ms)
  single-step of the SAME workload: >= 32000000 stops -> >= 94.3 s (proj., 1 instr/iter lower bound)
  => watchpoint has ~2000000x fewer stops than single-step here
```

**~23 ms (watchpoint) vs ≥ 94 s (single-step)** for the same work — a ≥ 4,000×
wall-time gap, and ~2,000,000× fewer stops. That is the whole point of F3: a
targeted data observation with essentially no perturbation window, versus
single-step's per-instruction trap density that widens the cross-thread deadlock
window on a live runtime.

## Read vs write (Phase D, real output)

Arming `R/W0=11` (read+write) trapped the loads the write-only arm did not — a
clean 2× on a victim that reads then writes the field once per round:

```
# Phase D — read+write watchpoint (R/W0=11) vs write-only (R/W0=01)
  write-only (DR7=0x90001): 8 hits (the 8 stores)
  read+write (DR7=0xb0001): 16 hits (the 8 stores + 8 loads)
  => R/W=11 also trapped LOADS: yes
```

`DR6` reports only **which slot** fired (B0..B3), **not** the access direction —
so labelling a `R/W=11` hit as read-vs-write needs a one-instruction decode at
the faulting site (see limits below).

---

## The DR7 encoding (validated)

The execution breakpoint the repo ships is `DR7 = 0x1` (`L0=1`, `R/W0=00`,
`LEN0=00`). A data watchpoint changes only the per-slot `R/W` and `LEN` fields,
reached through the same `struct user` `u_debugreg[]` / `PTRACE_POKEUSER` door:

| slot n field | bits          | values                                        |
|--------------|---------------|-----------------------------------------------|
| `Ln`         | `2n`          | local-enable slot n                            |
| `R/Wn`       | `16+4n..17+4n`| `00` execute · `01` **write** · `10` I/O · `11` **read+write** |
| `LENn`       | `18+4n..19+4n`| `00` 1B · `01` 2B · **`11` 4B** · **`10` 8B** (note: not monotonic) |

Validated DR7 words (printed by the probe, cross-checked by hand):

- **write, 8-byte, slot 0:** `L0 | (01<<16) | (10<<18)` = **`0x90001`**
- **read+write, 8-byte, slot 0:** `L0 | (11<<16) | (10<<18)` = **`0xB0001`**

The status read after each stop is `DR6` (`PTRACE_PEEKUSER` at `DR_OFFSET(6)`);
its low nibble `B0..B3` says which slot tripped. The probe clears `DR6` after
each hit (best-effort) so a multi-slot arm stays unambiguous.

## Caps (documented + enforced by the probe)

- **4 watchpoints max** — DR0..DR3, one address each. Per-**thread** (like the
  existing `set_hw_bp`): each thread has its own DR bank.
- **Lengths 1 / 2 / 4 / 8 bytes only**, and the watched address **must be
  length-aligned** (an x86 hardware requirement; the probe rejects a misaligned
  or bad-length arm with `-2`). A field wider than 8 bytes, or an unaligned span,
  needs multiple slots or a covering-aligned range.
- **A hit is a TRAP after the access retires.** The captured value is therefore
  the **post-write** value, and RIP is the **instruction following** the access
  (`+0x43` here is the insn after the store). Function-granularity symbolization
  is exact (RIP is inside the accessing function); pinning the exact access
  *instruction* needs decoding one instruction back from RIP.
- **Self-skips** where arming is refused (`POKEUSER` → `-1`) or **armed but never
  trips** (0 hits when hits were expected → the qemu-user "zero debug slots"
  case). The AArch64 build self-skips at compile time with the design note.

---

## Recommended landing shape

> **Resolution 2026-07-21: the x86 half landed as specified.**
> `asmspy --watch <pid> <sym|sym+off|0xADDR> [--rw] [--len=…]` ships in
> [cli/asmspy.c](../../../cli/asmspy.c) (~:5457 dispatch, ~:5171 usage, ~:1609
> JSON schema; ~:1668 arms every tid). The AArch64 `NT_ARM_HW_WATCH` analog below
> has **not** landed in `src/` — F3-arm64-watch remains genuinely open, consistent
> with [implementations/_positions.md](../implementations/_positions.md) #5.

A new near-zero-perturbation asmspy mode, exactly as the plan sketches
([live-attach-dataflow-followup-plan.md](../archive/plans/live-attach-dataflow-followup-plan.md), F3):

```
asmspy --watch <pid> <addr | func+off | &symbol> [--rw] [--len 1|2|4|8]
```

- **Arm:** `PTRACE_SEIZE`/`ATTACH` the target, resolve the location (a raw
  address, or `func+off` via the existing symbolizer / code-image recorder, or a
  DWARF/`.dynsym` symbol), reject a misaligned or over-length request, and
  `PTRACE_POKEUSER` `DR0` + the `DR7` word (`0x90001`-shape for `--rw` off,
  `0xB0001`-shape for `--rw` on; pick `LEN` from `--len`). Reuse the DR7/DR6
  plumbing this spike validated verbatim.
- **Per-thread arming is mandatory for a live multi-threaded process.** Debug
  registers are per-thread, so to catch *any* thread touching the field, iterate
  `/proc/<pid>/task/*` and arm every TID (and arm newly-created threads via
  `PTRACE_O_TRACECLONE`). The probe's single-threaded victim sidesteps this; a
  real landing must not.
- **At each `#DB`:** read `DR6` for the slot, `process_vm_readv` the watched
  bytes for the value, `GETREGS` for RIP, symbolize RIP to `function+offset` (the
  "who"). For `--rw`, decode the one instruction ending at RIP to label
  read-vs-write and, if wanted, extract the exact operand — the same
  operand-enumeration asmspy already does for its single-step value path
  ([`src/dataflow_ptrace.c`](../../../src/dataflow_ptrace.c)). Emit a row
  `{ts, tid, pc→func+off, dir, value}` to the Data-flow window / `--dataflow`
  JSON.
- **Contrast to keep in the UI:** this is the *targeted* data-flow view — 4
  fields, whole-process, native speed — complementary to the *whole-region*
  single-step/replay value trace (F1) and the whole-process DR taint tier. Same
  hand-off boundary the plan documents.

### AArch64 analog (`NT_ARM_HW_WATCH`)

Mirror the landed `NT_ARM_HW_BREAK` execution path
([`ptrace_backend.c:502-550`](../../../src/ptrace_backend.c#L502)) with the
**watchpoint** regset:

- `PTRACE_GETREGSET`/`SETREGSET` with `NT_ARM_HW_WATCH` and `struct
  user_hwdebug_state` (`dbg_info` + `dbg_regs[]`), exactly like the breakpoint
  path but a different note type.
- Per-slot `DBGWCR` control word: `E` (enable), `PAC=0b10` (EL0/user),
  `LSC` (bits 4:3 — **load** `01`, **store** `10`, **both** `11`, the direct
  R/W-mode analog), and `BAS` (byte-address-select, the length/offset within an
  8-byte-aligned `DBGWVR`), with `DBGWVR` holding the (aligned) watched address.
- **Same capability self-probe:** `dbg_info & 0xff` is the watchpoint-slot count;
  it is **0 under qemu-user**, so the AArch64 path self-skips there identically
  to the breakpoint path — no new gating logic needed.

---

## Limits (honest)

- **Read/write direction is not free.** `R/W=11` traps both, but `DR6` doesn't
  say which; a one-instruction decode at the faulting site is required to label
  it (and to name the exact source/dest operand). Write-only (`R/W=01`) is
  self-labelling (every hit is a store) and is the higher-signal default.
- **Post-access, not pre.** You observe the value *after* a write and the PC
  *after* the access. For "the value being written" this is correct; for "the
  value about to be read" the loaded value is still what's in memory at the stop,
  so `process_vm_readv` still returns it — but the *store that produced* it is a
  separate (earlier) event.
- **4 slots, per thread.** Watching more than 4 fields, or a >8-byte structure,
  needs slot multiplexing; watching across threads needs per-TID arming (above).
- **Perturbation is near-zero but not zero.** Each real hit is a full ptrace
  stop (~µs). On a field written in a hot inner loop, the hit *rate* — not the
  between-hit speed — becomes the cost; the primitive shines on
  infrequently-touched fields (config, a specific object slot, a suspected
  corruption site), which is the targeted use case F3 is scoped to.
- **Alignment.** An unaligned or awkwardly-sized field must be covered by an
  aligned length (e.g. watch the aligned 8 bytes containing a misaligned 4-byte
  field, then filter by offset), or rejected.

## Reproduce

```
gcc -O2 -Wall -Wextra examples/watchpoint_spike.c -o /tmp/watchpoint_spike
/tmp/watchpoint_spike ; echo "exit=$?"
```

Expected on x86-64 bare metal: `values MATCH`, `stops=16`, `all-PCs-identical=yes`,
`R/W=11 also trapped LOADS: yes`, `VERDICT: GO`, exit 0. Under qemu-user or where
`PTRACE_POKEUSER` on the debug registers is refused: a single `# SKIP` line, exit
0 (design findings above still stand).
