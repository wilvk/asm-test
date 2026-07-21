# Findings: DynamoRIO extension-load probe (taint tier, Increment 2)

*Status: findings / empirical record. Produced by
[dynamorio-taint-tier-plan.md](../archive/plans/dynamorio-taint-tier-plan.md) **Increment 2**,
whose whole purpose is to turn the long-documented "the prebuilt DR extensions won't
load under the private loader on modern glibc" claim into an empirical yes/no and to
settle the `umbra`/`drx_buf` license question the tier's LGPL-clean claim rests on.
The probe itself is [drclient/probe_extensions.c](../../../drclient/probe_extensions.c);
it runs in the throwaway `make docker-drext-probe` lane and the CI `drext-probe` job.*

## The question

The shipped DR clients ([drtrace_client.c](../../../src/drtrace_client.c),
[dataflow_dr_client.c](../../../src/dataflow_dr_client.c)) use **only** DynamoRIO's raw
BSD core API. The stated reason, recorded verbatim in
[drclient/CMakeLists.txt:19-21](../../../drclient/CMakeLists.txt#L19) and the client
header, is that the prebuilt release **extensions** (`drmgr`/`drreg`/`drx`/…) "fail to
load under DR's private loader on modern glibc." Every in-repo mention of this blocker
is generic — no glibc version boundary was ever pinned, and no build-from-source /
static-link / version-pin fix was ever attempted. The entire Phase-5 taint re-platform
(inlined `drmgr`/`drreg`/`drx_buf` instrumentation) is gated on it.

The plan framed four options in preference order: **(a)** build the extensions from
source against the pinned DR; **(b)** static-link them into the client; **(c)**
version-pin — keep the prebuilts if the loader probe passes as-is; **(d)** accept
`drwrap`'s LGPL-2.1 (rejected — the target stack does not need `drwrap`). It said to run
the cheap **(c)** probe first.

## Method

[probe_extensions.c](../../../drclient/probe_extensions.c) is a DR client that
`use_DynamoRIO_extension()`s the stack, calls one real API from each
(`drmgr_init` + a bb instrumentation event; `drreg_init` + a scratch-register
reserve/unreserve in the insertion phase; `drx_init` + `drx_buf_create_trace_buffer`),
counts instrumented instructions via a clean call, and prints a load-success line per
extension plus the final count. It runs under `drrun -c <probe>.so -- /bin/true` in the
pinned DR image (`DR_VERSION=11.91.20630`, the same pin as
[Dockerfile.drtrace](../../../Dockerfile.drtrace) and
[Dockerfile.drtrace-lang](../../../Dockerfile.drtrace-lang)). The lane records the
runner glibc first. `umbra` is behind an opt-in `-DPROBE_UMBRA` (see the license finding
below) and is **not** part of the committed gate.

Reproduce:

```
make docker-drext-probe                 # BSD-clean gate: drmgr + drreg + drx
docker run --rm asmtest-drext-probe \    # opt-in umbra load-check (informational)
    make drext-probe DYNAMORIO_HOME=/opt/dynamorio PROBE_UMBRA=1
```

## Result — the blocker does NOT reproduce; option (c) wins

On **glibc 2.39** (Ubuntu 24.04, `ldd (Ubuntu GLIBC 2.39-0ubuntu8.7)`), DR
**11.91.20630**, the prebuilt extensions load cleanly under the private loader:

```
drext-probe: drmgr loaded
drext-probe: drreg loaded
drext-probe: drx loaded
drext-probe: drx_buf trace-buffer created
drext-probe: instrumented 130588 instructions
drext-probe: PROBE OK (drmgr+drreg+drx)
```

- **Chosen option: (c) version-pin.** The prebuilt `drmgr`/`drreg`/`drx` load as-is
  under the pinned DR release on the CI image's glibc — no build-from-source (a) and no
  static-link (b) is required. The re-platform (Increment 3) can adopt the extension
  stack directly.
- **`drx_buf` is not a separate extension.** There is no `drx_buf.h` and no
  `libdrx_buf.so`; the buffered-trace API (`drx_buf_create_trace_buffer`,
  `drx_buf_insert_load_buf_ptr`, `drx_buf_free`, …) is declared in `drx.h` and built into
  `libdrx.so`. "Use `drx_buf`" means "use `drx`."
- **The `__memcpy_chk` symptom was not reproduced.** The concrete private-loader symbol
  failure the plan cited (from the macOS-DR notes) did not occur at this DR + glibc
  combination.
- **Boundary caveat.** This establishes only that the blocker is **absent at glibc 2.39
  with the pinned DR 11.91.20630** — the combination the tier actually ships against. It
  does **not** find a lower glibc boundary where an older DR build might have failed; the
  original claim may have been true for a different (older, unpinned) DR/glibc pair. The
  actionable conclusion is that the pinned combination is clean, so no workaround is
  owed. If the DR pin or base image is bumped, re-run this lane.
- **Non-zero instruction count** (130588 over `/bin/true`) confirms the `drmgr` phased bb
  instrumentation event actually fired, not merely that the libraries linked.

## Result — the license split (this contradicts the plan's assumption)

The plan's Increment 2 required confirming `umbra`/`drx_buf` are BSD/permissive, because
the tier's whole "stay LGPL-clean by avoiding only `drwrap`" posture depends on it. The
finding is a **split**, and it is not what the plan assumed:

| Extension | Ships under | License | Clean for the tier? |
|---|---|---|---|
| `drmgr` | `ext/` (DR core) | **BSD** (DR primary license, `License.txt`) | ✅ yes |
| `drreg` | `ext/` (DR core) | **BSD** | ✅ yes |
| `drx` (incl. the `drx_buf` API) | `ext/` (DR core) | **BSD** | ✅ yes |
| `umbra` | `drmemory/drmf/` (**Dr. Memory Framework**) | **LGPL-2.1** | ❌ no — same obligation as `drwrap` |

- `umbra` is **not** a DR-core `ext/` extension. It ships under
  `drmemory/drmf/` as part of the **Dr. Memory Framework**, whose `license.txt` states
  *"Primary license: LGPL"* (LGPL-2.1). `umbra.h` itself carries the LGPL-2.1 header.
  The **only** BSD carve-outs in that framework are the `drfuzz` and `drltrace` modules —
  **umbra is not among them**.
- So `umbra` is LGPL-2.1, exactly like `drwrap` — the one extension the tier
  deliberately refuses. The plan's "confirm `umbra`/`drx_buf` BSD/permissive" check
  resolves to: **`drx_buf` (via `drx`) is BSD ✅; `umbra` is LGPL-2.1 ❌.**
- For the record, the DRMF private-loader path *does* work — the opt-in
  `PROBE_UMBRA=1` run prints `umbra loaded (LGPL-2.1 — informational)` and completes.
  Loadability is not the umbra problem; **licensing** is.

## Implications for the re-platform

1. **Increment 3 (re-platform onto inlined instrumentation) is unblocked.** `drmgr` +
   `drreg` + `drx`/`drx_buf` are BSD and load under the pinned DR — the tier can move off
   the clean-call recorder onto the extension stack with no loader workaround and no
   license cost.
2. **The `umbra`-based byte-granular tag shadow (Increments 4 and 7) carries an LGPL-2.1
   obligation the tier is built to avoid.** This is a real decision, not a footnote, and
   the plan must own it rather than assume permissive umbra:
   - **(i) Hand-roll a BSD-licensed direct-mapped shadow** (DR core `dr_raw_mem_alloc`
     + a scale-down address map) instead of `umbra`. Keeps the tier fully BSD; costs the
     shadow-memory engineering `umbra` would have donated. *Recommended default* — it
     preserves the LGPL-clean invariant the whole tier is designed around.
   - **(ii) Accept LGPL-2.1 for `umbra`.** As a dynamically-linked, unmodified library
     the LGPL-2.1 relink obligation is generally satisfiable, but it drops the tier's
     "avoid the one LGPL extension" invariant, and static-linking `umbra` would inherit
     the stricter relink obligation. Only worth it if hand-rolling the shadow proves
     materially harder than expected.
   Either way, the plan's blanket "options (a)/(b)/(c) keep the tier LGPL-clean **as
   long as we avoid drwrap**" claim is **too weak**: it must additionally avoid `umbra`
   (or accept LGPL for it). Updated in the plan's License-posture section and Increment 4.
   *(Pointer, 2026-07-21: the "built to avoid umbra" stance still holds by default, but
   opt-in compliance machinery to include LGPL-2.1 `drwrap`/`umbra` under the project's
   MIT terms was later added — commit `0a76717`,
   [licenses/LGPL-2.1.txt](../../../licenses/LGPL-2.1.txt). The finding itself is
   unchanged.)*
3. **No change to the shipped Increment-1 client.** It stays on the raw core API,
   untouched, as the oracle/fallback until Increment 3 regression-proves its replacement
   byte-for-byte. `make drtrace-client` (both shipped `.so`s) was reconfirmed to build
   through the (additively) edited `drclient/CMakeLists.txt`.

## Artifacts landed by this increment

- [drclient/probe_extensions.c](../../../drclient/probe_extensions.c) — the throwaway
  probe (BSD stack default; `-DPROBE_UMBRA` opt-in).
- [drclient/CMakeLists.txt](../../../drclient/CMakeLists.txt) — opt-in
  `ASMTEST_BUILD_DREXT_PROBE` / `PROBE_UMBRA` targets; shipped clients unchanged.
- [Dockerfile.drext-probe](../../../Dockerfile.drext-probe) + `make docker-drext-probe`
  + `make drext-probe` ([mk/native-trace.mk](../../../mk/native-trace.mk),
  [mk/docker.mk](../../../mk/docker.mk)).
- CI `drext-probe` job ([.github/workflows/ci.yml](../../../.github/workflows/ci.yml)) —
  fails red if any of `drmgr`/`drreg`/`drx` fails to load or zero instructions are
  instrumented.
