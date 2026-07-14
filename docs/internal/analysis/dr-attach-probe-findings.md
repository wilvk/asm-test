# DR external-attach probe — findings (DR ATTACH tier, Increment 2)

**Verdict: GO (2026-07-14).** DynamoRIO's EXPERIMENTAL external attach to an
**already-running native process** works on the pinned DR **11.91.20630** on Linux
x86-64: `drrun -attach <pid> -c <client>` takes the running process over, the client's
`drmgr` bb-instrumentation event fires over the victim's live code (**1,600,140,350
instrumented instructions executed** in the probe), the victim **keeps running** (its
heartbeats continued past the attach), and it **exits native** cleanly. This is the
empirical yes/no the plan flagged as *the experimental blocker* — it **unblocks
attach-tier Increments 3-5**.

## What was probed

The extension-load-probe move, for attach. Artifacts (all throwaway, not product):
- `examples/attach_probe_victim.c` — a plain native process (a bounded ~12 s heartbeat
  loop doing real integer work), started FIRST and left running.
- `drclient/attach_probe.c` — a minimal `drmgr`-only client that counts instrumented
  instructions EXECUTED via a clean call and prints `ATTACH_PROBE_TAKEOVER_OK` iff the
  count is non-zero (proof the takeover reached the victim's live execution, not merely
  that DR decoded the code).
- `make dr-taint-attach-probe` / `make docker-taint-attach-probe` — starts the victim,
  lets it run natively (~2 s, counting pre-attach heartbeats), injects DR + the client
  into the RUNNING pid via `drrun -attach`, waits for the victim to finish, and prints
  `ATTACH PROBE OK` (GO) iff: the client reached `dr_client_main`, instrumented a
  non-zero instruction count, the victim's heartbeats CONTINUED past the attach
  (survival), and the victim reached `VICTIM_END` (exited native).

Measured (containerized, `--cap-add=SYS_PTRACE`):
`client_reached=1 takeover_ok=1 pre_beats=20 post_beats=100 victim_end=1 attach_rc=0
victim_rc=0`.

## Load-bearing gotchas (record for Increments 3-5)

1. **`-attach <pid>` must precede `-c <client>` on the drrun command line.** drrun parses
   everything after `-c <client>` as CLIENT options (up to a `--`), so
   `drrun -c <client> -attach <pid>` silently consumes `-attach <pid>` as client args and
   then fails with `ERROR: no app specified` (drrun thinks it is a launch with no app).
   The working form is `drrun -attach <pid> -c <client>` — `-attach` is a drrun option and
   must land in the drrun-option position, and no `-- <app>` is given (attach does not
   launch). This one-line ordering mistake reads as "external attach unsupported" if you
   trust the first error; it is not. `drrun -h` DOES list `-attach <pid>` ("Attach to the
   process with the given pid. Attaching is an experimental feature…").
2. **CAP_SYS_PTRACE is required and SUFFICIENT — it bypasses `yama/ptrace_scope`.** The
   container reported `ptrace_scope=1` (a non-descendant process may not normally be
   ptraced), yet the attach succeeded because `--cap-add=SYS_PTRACE` grants CAP_SYS_PTRACE,
   which overrides the yama restriction. No `--security-opt seccomp=unconfined` was needed
   (Docker's default seccomp allows `ptrace` once the cap is present). The attach injector
   (drrun) and the victim are siblings (both children of the make shell), not
   parent/child, so without the cap the seize would be blocked under `ptrace_scope=1`.
3. **The API surface exists in the pinned DR.** `dr_inject.h` exposes
   `dr_inject_process_attach(pid, &data, &app_name)` and `dr_inject_prepare_to_attach(...)`,
   and `libdrinjectlib.a` ships — so a custom in-tree injector (instead of the `drrun`
   front-end) is also available if a later increment needs finer control (e.g. `-late`
   vs early attach, or attaching without spawning the drrun process). The `drrun -attach`
   front-end is the simplest path and it works, so Increments 3-5 can start there.
4. **The victim is unharmed by a FAILED attach attempt.** The first (mis-ordered) run left
   the victim running to a clean `VICTIM_END` — drrun errored out before touching it. So
   the harness distinguishes "attach failed" from "attach crashed the victim."

## Consequences

- Increment 2's go/no-go is **GO** — the attach tier's central risk (DR external-attach
  maturity on the pinned version) is retired for the NATIVE case.
- Increments 3-5 (marker-less config, native data-flow/taint end-to-end, detach
  correctness + cycling) can build on `drrun -attach <pid> -c <taint client>` reusing the
  UNCHANGED taint client + shm channel + out-of-process validator, exactly as the
  cooperative (Increment 1) and launch (taint Increment 5) paths do.
- MANAGED attach (Increment 6) stays research-gated — this probe covers native only; the
  arbitrary-safepoint-state problem for a running .NET runtime is out of scope here.
- The probe is a **manual diagnostic** (`make docker-taint-attach-probe`), not wired into
  the main CI gate: it needs `--cap-add=SYS_PTRACE`, and it characterizes an experimental
  DR feature rather than gating the product.
