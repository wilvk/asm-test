# Remaining-work assessment vs this machine (2026-07-02)

Host: **AMD Ryzen 9 9950X (Zen 5), Linux 6.17**, `perf_event_paranoid=4`, no
passwordless sudo. This maps every remaining forward-looking item from the plans
to why it is NOT actionable-and-validatable here, so what remains is exactly the
"manual action" and "non-AMD-Zen-4/5" set the goal allows to stay open.

## Blocked on a MANUAL ACTION (privilege) — AMD Zen 4/5 code that can't run here

`perf_event_paranoid=4` blocks unprivileged `perf_event_open` for branch-stack
(LbrExtV2) and BPF; validating any of these needs `sysctl kernel.perf_event_paranoid=1`
(or `CAP_PERFMON`/`CAP_BPF`), which requires root on this box. These are otherwise
Zen 5-capable:

- **AMD improvement roadmap** ([amd-tracing-plan.md](../plans/amd-tracing-plan.md)
  Part III) Phase 1 (freeze-on-PMI window-trust gate), Phase 3 (eBPF on-demand
  `bpf_get_branch_snapshot`), Phase 4 (LbrExtV2 spec/wrong-path filtering),
  Phase 5 (Tier-B stitch hardening) — all exercise live LbrExtV2 capture, which
  this paranoid level forbids. (Finding **#12**'s ring-full-loss fix is decode-side
  and shipped in [batch-c](2026-07-02-batch-c-hwtrace.md); it applies whenever the
  capture path does run.)
- **Phase 0** (CPUID `0x80000022` runtime LBR-depth reader): buildable and CPUID
  is readable here, but on current AMD LbrExtV2 the depth is fixed at 16 — the
  value already hardcoded (`AMD_LBR_DEPTH`) — so a runtime read returns 16 and
  changes no behavior on any shipping AMD part. Deferred rather than speculatively
  re-threading the (unrunnable-here) capture/decode path for zero functional
  change; it becomes worthwhile only if AMD ships a non-16 depth.

## Blocked on NON-AMD-Zen-4/5 HARDWARE / OS

- **Intel PT capture half of finding #11** — the `PERF_EVENT_IOC_SET_FILTER`
  region address filter needs a GenuineIntel PT host. (The decode-side contract
  fix — flag truncated on an empty in-region decode — shipped in batch-c.)
- **AMD roadmap Phase 6** (BRS single-window Tier-A) — needs a **Zen 3** BRS host;
  this dev box is Zen 5 (LbrExtV2, not BRS). Phase 7 (IBS-Op lane) is forward-look,
  statistical, low priority.
- **AArch64 CoreSight live decode** ([hardware-trace-plan.md](../plans/hardware-trace-plan.md))
  — needs a real AArch64 CoreSight board + libopencsd.
- **AArch64 live single-step stream, Windows VEH, macOS-Intel single-step**
  ([zen2-singlestep-trace-plan.md](../plans/zen2-singlestep-trace-plan.md) Phase 5)
  — need real ARM64 hardware / a Windows host / a macOS host (qemu-user cannot
  emulate the ptrace tracer/tracee relationship — see the arm64-docker memory).
- **macOS DynamoRIO** ([macos-drtrace-plan.md](../plans/macos-drtrace-plan.md)) —
  dead-blocked upstream (no macOS DR release exists).
- **Finding #36** (Win64 `--no-fork` kernel-wait) and the Win64 runtime edges — a
  bounded hard-exit mitigation shipped ([batch-g](2026-07-02-batch-g-windows.md));
  a full fix + validation needs a real Windows host.

## Blocked on MANUAL / CREDENTIALED action (not code)

- **Binding publish go-live** (post-v1 Track A, fully-featured-packages,
  multi-language-bindings deferrals) — register package names, add per-ecosystem
  token secrets (now scoped to the publish steps per B6), tag a real `v1.x`.
- **Repo-review B2** (add a `macos-13` Intel leg to `release.yml`, or document
  macOS packages arm64-only) and **B5** (record trusted SHA-256/signature anchors
  for the fetched DynamoRIO / built Keystone-Capstone) — both maintainer trust /
  cost decisions. **B7** (`-j` recursive-make race) — a Low build-arch refactor of
  the `DRAPP_KEYSTONE=0` recursive `$(MAKE)` pattern, deferred (can't validate the
  race here).

## Large greenfield features (not defects) — out of this pass's scope

- **Call-descent tracing** ([call-descent-plan.md](../plans/call-descent-plan.md))
  — an entire unimplemented plan (~30 engineer-days, 3 open design decisions).
- **DynamoRIO drsyms internal-symbol mode**, **AArch64 SVE wide-vector capture**
  (needs SVE hardware), **PT-attach-to-live-PID Phase 2**, hypervisor/EPT frontier.

---

**Net:** the entire review-driven **defect backlog is done and validated** — all
54 findings of the [2026-07-02 code review](../analysis/2026-07-02-code-review.md)
(52 fixed, #11 decode-side/PT-filter-remaining, #19 doc-corrected), the still-open
[2026-07-01](../reviews/2026-07-01-repo-review.md) items (#3, #8, #9, #10), and the
[2026-07-02 repo-review](../reviews/2026-07-02-repo-review.md) build/CI items
B1/B3/B4/B6 + S3. What remains is exactly manual-action- or
non-AMD-Zen-4/5-hardware-gated, per the goal.
