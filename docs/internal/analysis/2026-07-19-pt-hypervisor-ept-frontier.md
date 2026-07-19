# The hypervisor / EPT frontier for maximum-stealth PT tracing — design record

> **Status: forward-look design record, no shipping code.** This note is the
> deliverable of [intel-pt-attach-foreign-pid.md](../implementations/intel-pt-attach-foreign-pid.md)
> **T5** (HWT-HV-EPT). It records the API surface and the wiring decision for the
> maximum-stealth Phase-2 option so a future implementor starts from verified
> facts rather than re-researching. It ships **no source, Makefile, or Dockerfile
> change** — nothing in the tree references `vmtrace`, `drakvuf`, `altp2m`, or
> `libxdc`, and that stays true until the gate below is met. The verification that
> replaces a test is: this note cites the primary sources listed at the end and is
> reviewed against them.

## Why this is a separate, later frontier

The in-tree PT work traces a process from *inside the same OS* — the self-trace
window pair and the foreign-pid attach
([intel-pt-attach-foreign-pid.md](../implementations/intel-pt-attach-foreign-pid.md)
T1–T4) both open an `intel_pt` perf event against a pid on the host. The
frontier here is **out-of-VM**: trace (or breakpoint) a *guest's* execution from
the host/hypervisor, so the target cannot observe the tracer at all — no perf
event in its namespace, no `0xCC` it can read back. That needs a hypervisor, not
a syscall, so it cannot begin until (a) the PT-attach slice (T1–T4) ships the
decode/report plumbing it reuses, and (b) a bare-metal Intel host running Xen is
available. Neither dev box provides the latter.

These are **two distinct mechanisms**, not one fused technique — state that
plainly (global-position discipline: do not imply a single "PT + altp2m"
primitive):

1. **Host-side Intel PT of a guest's execution** via Xen `vmtrace` — a *tracing*
   primitive (what ran).
2. **altp2m execute-only EPT views for hidden breakpoints** — a *concealment*
   primitive (a breakpoint the guest cannot read). DRAKVUF happens to expose
   both, but they are independent.

## 1. Xen `vmtrace` — host-side Intel PT of a guest

- **Available since Xen 4.15** (April 2021), behind a build with `CONFIG_VMTRACE`.
- Control hypercall: **`XEN_DOMCTL_vmtrace_op`** (enable / disable / reset /
  query output position, per vCPU).
- The per-vCPU trace buffer is mapped into dom0 via
  **`XENMEM_acquire_resource(XENMEM_resource_vmtrace_buf)`** /
  `xenforeignmemory_map_resource`; its size is fixed at **domain-create time**
  via the `vmtrace_buf_size` (guest config `vmtrace_buf_kb`).
- libxc wrappers: **`xc_vmtrace_enable` / `_disable` / `_set_option` /
  `_output_position`**.
- **Constraints:** x86 **HVM + Intel VT-x only** — AMD-V and PV guests are
  unsupported. Xen emits **raw Intel PT packet bytes only**; decode is done
  **off-host** with a PT decoder — the *same* libipt path this project already
  owns (`src/pt_backend.c` / `asmtest_pt_decode_window`) and the same Capstone
  layer used to render recovered bytes. So the decode/report half is a
  straight reuse; only the *capture* half (the `xc_vmtrace_*` producer) is new.

## 2. DRAKVUF — the `ipt` plugin and altp2m execute-only EPT

- **`ipt` plugin** (`-a ipt`): post-processes a guest's Intel PT with
  **libipt + Intel `xed`** (a modified `ptxed`). Needs **Xen ≥ 4.15**, supports
  up to **16 vCPUs**, and wants a generous buffer (`vmtrace_buf_kb = 8192` in the
  domain config). This is DRAKVUF's consumer of mechanism (1).
- **altp2m execute-only EPT** (`xc_altp2m_*`): the concealment mechanism. It
  **duplicates the code page**, writes the `0xCC` breakpoint **only into the
  shadow copy**, and marks the breakpoint-bearing page **execute-only** in EPT —
  so a guest *read* of that page traps and is served the clean (un-patched) view,
  while *execution* hits the shadow `0xCC`. The guest cannot see its own
  breakpoints. This is orthogonal to PT: you can use altp2m breakpoints without
  PT, or PT without altp2m.

## 3. Decoder trade — `libxdc` vs `libipt`

- **`libxdc`** (nyx-fuzz) claims **15–30× over libipt** for fuzzing workloads
  (full-speed PT decode with a cached edge map).
- **But `libxdc` builds against Capstone v4**, while this repo **pins Capstone
  5.0.1** ([scripts/third-party-digests.txt](../../../scripts/third-party-digests.txt)).
  Adopting `libxdc` would need a **separate Capstone-v4 build** and could not
  reuse the pinned Capstone — a real maintenance and provenance cost.
- **Recommendation: stay on libipt** unless a *measured* need appears (a
  fuzzing-scale PT decode throughput requirement). libipt is already vendored
  (`libipt-dev` 2.0.6 in [Dockerfile.hwtrace](../../../Dockerfile.hwtrace)) and
  drives every PT decode in the tree today.

## The gate

This frontier is gated on **all** of:

- **Xen 4.15+** built with `CONFIG_VMTRACE` (for `vmtrace`) — and the DRAKVUF
  stack (Xen + libvmi + drakvuf) for the altp2m/`ipt` path.
- **Intel VT-x HVM** guests — an **Intel host** (AMD-V and PV are unsupported by
  `vmtrace`). Both dev boxes are AMD; no Xen host is reachable.
- **Bare-metal Intel PT** on that host (the same silicon gate as the in-tree PT
  lanes).

Until every one is met this stays a design record. When it is met, the
implementation reuses the existing libipt decode + Capstone render + the
`asmtest_trace_t` report format; the new surface is only the `xc_vmtrace_*`
capture producer (mechanism 1) and, separately, an `xc_altp2m_*` hidden-breakpoint
driver (mechanism 2).

## Primary sources (verified 2026-07-17)

- Xen `vmtrace` — [Xen 4.15 release](https://xenproject.org/2021/04/08/xen-project-hypervisor-4-15),
  the `xc_vmtrace_*` / `XEN_DOMCTL_vmtrace_op` /
  `XENMEM_resource_vmtrace_buf` series
  ([patch series](https://patchew.org/Xen/20210121212718.2441-1-andrew.cooper3@citrix.com/20210121212718.2441-9-andrew.cooper3@citrix.com/)).
- DRAKVUF — [Intel Processor Trace plugin](https://github.com/tklengyel/drakvuf/wiki/Intel-Processor-Trace)
  (`-a ipt`, `vmtrace_buf_kb`, ≤16 vCPUs, libipt+ptxed) and
  [Xen altp2m](https://github.com/tklengyel/drakvuf/wiki/Xen-altp2m)
  (execute-only EPT shadow-page breakpoints) — two distinct mechanisms.
- Decoder — [libxdc](https://github.com/nyx-fuzz/libxdc) (15–30× over libipt,
  Capstone v4).
