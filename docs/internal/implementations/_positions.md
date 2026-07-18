# Global consistency positions — binding on every implementation doc

The extraction pass found 12 places where source docs contradict each other or
the code. These are the resolved positions. Every implementation doc MUST take
these positions; never repeat the refuted claim, even when quoting a source.

1. **`trace_call_auto` AMD-LBR completeness bug is FIXED** (commit 5d8e0d2).
   `docs/internal/amd-hardware-validation.md` saying "not yet fixed" is stale —
   the `amd-ibs-backend-honesty` doc owns rewriting it. On AMD validation,
   `truncated=0` where escalation must fire is a REGRESSION signal, not "the
   known open finding".
2. **Zen 3 BRS was never opened by this tree.** `amd-tracing-plan.md` Phase 0's
   BRS-fallback bullet is marked landed but was never built (no
   `PERF_TYPE_RAW`/`0xc4` in `src/`). The AMD LBR live-capture floor is
   **Zen 4** until the raw-0xc4 arm ships; every "Zen 3+" claim in guides,
   headers, and the parity matrix is corrected to "Zen 4+" (docs task in
   `amd-branchsnap-lbr-docs`, which also owns the silicon-gated capture arm).
3. **`asmtest_amd_freeze_available` is dead code** (gate removed in 5d8e0d2,
   zero live call sites). Docs must not describe a working freeze gate;
   `amd-branchsnap-lbr-docs` retires or wires it and fixes the false
   PRESENT/ABSENT printf in `examples/test_hwtrace.c`.
4. **AMD Part III P2 (BTF block-step) and P3 (eBPF snapshot) are LANDED**,
   despite `scoped-tracing-zeroconfig-plan.md`'s "Consumed" table saying
   planned. Treat those primitives as available.
5. **The live-attach dataflow plan's "F5 alone is open" header is wrong.**
   F2-inc2, F3-arm64-watch, F7-slice, F7-codeimage, F4-object-identity, and
   F6-subreg-alias are all genuinely open; the implementation docs are the
   authority.
6. **macOS Intel CI legs use `macos-15-intel`, never `macos-13`** (retired
   2025-12-08). Applies to every CI-touching doc.
7. **DynamoRIO external attach IS wired for the taint/dataflow tier**
   (landed 2026-07-14: `Dockerfile.taint-attach`, `docker-dataflow-attach`);
   only the drtrace control-flow tier remains `dr_app_*`-cooperative. Pin
   docs must not premise their value on "DR attach is unwired".
8. **int3/si_code fix shape**: use the empirically-validated
   SI_KERNEL/TRAP_HWBKPT app-whitelist shape already encoded in
   `src/dataflow_ptrace.c:789-806`, NOT a naive TRAP_BRKPT split. Forward
   app breakpoints via PTRACE_CONT, never PTRACE_SINGLESTEP.
9. **One PT arm, owned by `intel-pt-whole-window-substrate`**: it builds the
   perf-AUX intel_pt open/mmap/drain/decode helpers, the begin_window PT arm
   (pid==0), the WEAK/STRONG/CEILING ladder, and the native
   `pt_begin_window`/`pt_end_window` pair the .NET inline ctor uses.
   `intel-pt-attach-foreign-pid` extends that same arm for foreign pids —
   no parallel PT implementation anywhere.
10. **IBS callchain**: the window lane must stop advertising
    PERF_SAMPLE_CALLCHAIN in `include/asmtest_ibs.h` (no consumer exists);
    gate callchain out of the window lane and make IBS_MAX_RECORD
    callchain-aware. Cite the archived plan at
    `docs/internal/archive/plans/zen2-ibs-tracing-plan.md` (not `plans/`).
11. **The Docker-OSX KVM lane is hardware-gated** on a bare-metal Linux box
    with `/dev/kvm`; the plan's "launchable today" note referred to a host
    this environment does not have. Self-skip off KVM.
12. **F4 GC-move canonicalization correctly shipped ADDRESS identity**;
    object identity (GCBulkType/Node/Edge) is the documented next increment,
    not a fix to a broken path. Do not describe the landed F4 as defective.

Cross-doc etiquette: reference sibling implementation docs as
`<slug>.md#T<n>`; do not restate or re-scope a sibling's tasks — link them
under "Out of scope".
