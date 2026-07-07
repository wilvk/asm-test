# asm-test — Bundle the DynamoRIO & hardware-trace tiers into the packages

Make the two native-trace tiers — **DynamoRIO** (`libasmtest_drapp` +
`libasmtest_drclient.so`) and **hardware trace** (`libasmtest_hwtrace`) — ship
*inside* the published language packages, so a fresh `pip install asmtest` (gem,
npm, nupkg, jar, rock) can run `NativeTrace`/`HwTrace` on a capable host **with no
manual `make shared-drtrace` / `shared-hwtrace` and no `DYNAMORIO_HOME`** — exactly
as the emulator/Keystone/Capstone tiers already do.

> Status legend: **done** / **planned**. Update as tracks land.
>
> **Status: implemented (all tracks). "Ship all" — both tiers bundled into the default
> payload on the Linux slots, hwtrace built with the full decoders where available.**
> Locally verified on `linux-x86_64`: hwtrace + drtrace stage with `$ORIGIN` rpaths, the
> bundled `libdynamorio` self-locates via `dladdr` (`asmtest_dr_available()` → 1 with no
> env), the DynamoRIO license is captured + emitted, and a clean-room Python simulation
> resolves both tiers from the package with no `build/` leak. The full multi-binding
> wheel/gem/... builds run in CI (they need the Unicorn/Keystone/Capstone toolchain).

---

## Context: what ships today, and what doesn't

The packaging pipeline bundles only the **core + superset** libraries. From
[`package-libs`](../../../../mk/bindings.mk) (`shared shared-emu`):

- `libasmtest` (capture) and `libasmtest_emu` (emulator + Keystone + Capstone) are
  copied into `build/dist/native/<plat>/`, then
  [`scripts/package-native.sh`](../../../../scripts/package-native.sh) vendors
  Unicorn/Keystone/Capstone next to the emu lib with an `$ORIGIN` / `@loader_path`
  rpath and [`collect-licenses.sh`](../../../../scripts/collect-licenses.sh) writes
  `THIRD-PARTY-LICENSES/`.
- The multi-platform packers copy the whole slot: `emu_lib_slots` (Ruby/Node/Java/
  Lua) and the `.NET` RID loop both `cp lib*.so* lib*.dylib`; Python stages named
  files into `asmtest/_libs/` and lets auditwheel/delocate vendor deps at repair time.

The two native-trace libs are **built but never staged into a slot**, so they are
absent from every package. Each binding's `drtrace`/`hwtrace` wrapper dlopens its lib
from `$ASMTEST_DRAPP_LIB` / `$ASMTEST_HWTRACE_LIB` → **`_REPO_ROOT/build/`** → bare
system search ([`drtrace.py`](../../../../bindings/python/asmtest/drtrace.py#L63),
[`hwtrace.py`](../../../../bindings/python/asmtest/hwtrace.py#L186)) and self-skips
(`available()` → `false`) when nothing resolves. So on an installed package the tiers
are simply *off*.

This plan stages both libs into the payload, wires their runtime deps, teaches each
loader a **bundled-package candidate**, and extends the license + CI machinery — while
changing **no API and no `available()` semantics** (a bundled tier still self-gates on
hardware/OS; bundling only removes the *manual build* step).

### The two tiers have very different bundling cost

| | DynamoRIO (`drtrace`) | Hardware trace (`hwtrace`) |
|---|---|---|
| Libs to ship | `libasmtest_drapp.so`, `libasmtest_drclient.so`, **`libdynamorio.so`** | `libasmtest_hwtrace.so` |
| New vendored dep | **DynamoRIO runtime** (tens of MB, pinned `11.91.20630`) | **none** in the decoder-free build (reuses the already-vendored Capstone); libipt/OpenCSD only in a "full" build |
| Platforms | **Linux x86-64 only** | Linux x86-64 (full) + Linux arm64 (partial: `/proc`+jitdump readers live, single-step stream pending real hw) |
| License added | DynamoRIO core **BSD-3** (no drmgr/drwrap → no LGPL) | none new (Capstone already listed); libipt BSD, OpenCSD BSD-3, libbpf LGPL-2.1-or-BSD-2 only in "full" |
| Payload delta | **large** (~doubles the Linux slot) | **negligible** (one small `.so`) |

The asymmetry drives the sequencing: **hwtrace is nearly free and lands first**;
**drtrace is heavy and gated behind an explicit size decision.**

### What "all packages" actually means

Only **six** bindings ship a prebuilt native payload and are in scope:
**Python** (wheel), **Ruby** (gem), **Node** (npm), **Java** (jar), **.NET**
(nupkg), **Lua** (rock). The other four — **Rust, Zig, C++, Go** — are *source
distributions*: the consumer builds/links `libasmtest` themselves
([`mk/bindings.mk`](../../../../mk/bindings.mk) `rust-package`/`zig-package`/`cpp-package`/
`go-package`), so there is no payload to bundle into — for those, "bundling" means
**documenting** the extra `make shared-drtrace` / `shared-hwtrace` link/dlopen step
(Track 5), not shipping a binary.

Because `emu_lib_slots` and the `.NET` loop copy `lib*.so*` by **glob**, once a lib is
staged into `build/dist/native/<plat>/` the Ruby/Node/Java/Lua/.NET packers pick it up
**for free**. The concentrated work is in `package-libs` (staging), `package-native.sh`
(vendoring/rpath/symbol-assert), the **Python** wheel (named copies + auditwheel of a
standalone dlopen'd lib), the **loaders** (bundled candidate), the **licenses**, and
**CI** (build the tiers on the Linux runners).

---

## Bundling matrix (target end-state)

| Slot | `libasmtest_hwtrace` | `drapp`+`drclient`+`libdynamorio` |
|---|---|---|
| `linux-x86_64` | ✅ (Track 1) | ✅ (Track 3) |
| `linux-arm64` | ✅ partial — readers live, step self-skips (Track 1) | ❌ (DR tier is x86-64-only) |
| `darwin-arm64` / `darwin-x86_64` | ❌ (tier is Linux-only → wrapper self-skips) | ❌ |

On every slot without the lib, the wrapper's `available()` already returns `false`,
so the package is correct on macOS and on arm64 — it just can't offer the tier there.

---

## Track 0 — Loader precedence: teach every wrapper the bundled path — **done**

Groundwork shared by both tiers. Today the `drtrace`/`hwtrace` loaders look at
`$ENV → repo build/ → system`; a *bundled* package has the lib next to the core lib in
`_libs/` (Python) or the per-platform `native/<slot>/` dir (others), which is **not on
that list**.

- Insert a **bundled candidate** into each wrapper's search, at the right precedence:
  `$ASMTEST_*_LIB → bundled (package dir) → repo build/ → system`. Mirror the core
  loader's `_LIBS` logic ([`_native.py`](../../../../bindings/python/asmtest/_native.py#L36))
  in `drtrace`/`hwtrace` for all six payload bindings (and the four source bindings'
  dlopen paths, which resolve from the linked/installed prefix).
- **Self-report parity (ties into the [macOS clean-room plan](../../plans/macos-clean-test-plan.md)
  Track A).** Give the tier loaders the same "print the resolved path" accessor the
  core `--where` work adds, so a clean-room test can assert a bundled tier resolved
  from the package — not a leaked `build/` tree. The `_REPO_ROOT/build/` fall-through
  must sit **below** the bundled path so an installed package never prefers the dev tree.
- Keep the env override **first** (an advanced user still points at a hand-built lib).

## Track 1 — Bundle `libasmtest_hwtrace` (decoder-free) — **done**

The cheap, high-value win: the universal **single-step + out-of-process ptrace** tier
finally ships turnkey on every Linux package.

- **Build the bundled lib decoder-free.** With `LIBIPT_DEF=`, `OPENCSD_DEF` unset, and
  no `libbpf`, `shared-hwtrace` links only **Capstone** (already vendored) + `libc`/
  `libdl` — zero new vendored deps. The Intel-PT/AMD/CoreSight backends compile in
  their self-skip form and `available()` reports `false` for them off-hardware (the
  common case), while `SINGLESTEP` + the ptrace/`/proc`/jitdump paths work on any
  Linux host. This is the turnkey default.
- **Stage it in `package-libs`.** After `shared shared-emu`, also build
  `shared-hwtrace` and `cp libasmtest_hwtrace.so` into `build/dist/native/<slot>/` on
  the Linux slots only (guard on `PKG_PLAT` matching `linux-*`).
- **Extend `package-native.sh`.** Assert the lib exports a sentinel symbol
  (`asmtest_hwtrace_available`); patch its rpath to `$ORIGIN` so it finds the vendored
  Capstone already in the slot. (No new deps to copy in the decoder-free build.)
- **Ruby/Node/Java/Lua/.NET: free** via the `lib*.so*` glob. **Python:** add
  `cp libasmtest_hwtrace.so` into `asmtest/_libs/` and confirm auditwheel/delocate
  either vendors or leaves-resolvable a **standalone dlopen'd** `.so` that no extension
  module links (it is not on `libasmtest_emu`'s `DT_NEEDED`, so repair tools won't
  discover it transitively — stage it explicitly and verify it loads post-repair).
- **Licenses:** no change (Capstone already in `THIRD-PARTY-LICENSES`).

## Track 2 — "full" hwtrace (Intel PT / CoreSight decoders) — **done** *(shipped in the default Linux payload)*

For hosts with the PMU/decoders, bundle the decode libs so PT/CoreSight fidelity is
available without a rebuild. Bigger and optional — behind a build flag / package
variant, not the default.

- Build `shared-hwtrace` with `LIBIPT_DEF`/`OPENCSD_DEF` set; `package-native.sh`
  vendors **libipt** (no pkg-config — copy by `ldd` name) and **OpenCSD** with
  `$ORIGIN` rpath, mirroring the Unicorn/Keystone/Capstone path.
- Extend `collect-licenses.sh`: **libipt** (BSD), **OpenCSD** (BSD-3), and if the eBPF
  detector is included, **libbpf** (convey under its **BSD-2** option to avoid a new
  LGPL obligation). All permissive → no copyleft beyond the existing Unicorn/Keystone
  GPL-2.0 the package already carries.
- Decide packaging shape: a single "full" lib in the default payload vs. a separate
  `asmtest-hwtrace-full` extra. Recommend keeping the **decoder-free** lib as the
  default (works everywhere, zero deps) and offering the full lib as an opt-in slot.

## Track 3 — Bundle DynamoRIO (`drtrace`) — **done** *(shipped in the default `linux-x86_64` payload — "ship all")*

The heavy tier: ship `libasmtest_drapp.so` + `libasmtest_drclient.so` + the pinned
**`libdynamorio.so`**, `linux-x86_64` slot only.

- **Fetch + stage DynamoRIO.** Reuse the [`Dockerfile.drtrace`](../../../../Dockerfile.drtrace)
  pin (`DR_VERSION=11.91.20630`); `package-libs` (Linux-x86-64 slot) builds
  `shared-drtrace` + `drtrace-client` with `DYNAMORIO_HOME` set, then copies all three
  libs into the slot.
- **Vendor + rpath.** `package-native.sh` copies `libdynamorio.so` into the slot and
  sets `$ORIGIN` rpath on `drapp`/`drclient` so `drapp`'s runtime `dlopen` of
  `libdynamorio` and DR's load of the client both resolve inside the package (no
  `DYNAMORIO_HOME` needed at run time). Assert `asmtest_dr_available` is exported.
- **Loader:** Track 0 already added the bundled candidate; also point the client-path
  default (`$ASMTEST_DRCLIENT`) and DR-runtime default (`$ASMTEST_DR_LIB`) at the
  bundled copies when running from a package.
- **Licenses:** add **DynamoRIO** (BSD-3-Clause) to `collect-licenses.sh` — a permissive
  addition; the "no drmgr/drwrap → no LGPL-2.1" property is preserved and worth noting
  in the NOTICE.
- **Size decision (call it out explicitly).** `libdynamorio.so` is tens of MB and lands
  in *every* Linux payload of six packages. Options: (a) accept it in the default
  payload; (b) ship it only in a `-full` variant / optional download; (c) gate behind a
  package extra. **Recommendation:** given the size and the Linux-x86-64-only reach,
  make drtrace an **opt-in `-full`/extra payload**, not the default, unless the user
  wants it unconditionally in the base package.

## Track 4 — CI: build the tiers on the payload runners + clean-room assert — **done**

- **`native` job (`release.yml`, `ci.yml` payloads):** on the two Linux runners,
  install DynamoRIO (curl the pinned tarball) and — for the full hwtrace lane — libipt/
  OpenCSD, then have `package-libs` build+stage the tiers. macOS runners are unchanged
  (tiers are Linux-only). Keep `fail-fast: false`.
- **`package-libs-verify`** ([`mk/bindings.mk`](../../../../mk/bindings.mk)): extend the merged-
  tree check so each present tier lib carries an `$ORIGIN`/`@loader_path` rpath, no
  absolute/leaked path, and its license note; the darwin slots must **not** carry them.
- **Clean-room smoke:** after install (with `$ASMTEST_*_LIB` unset), assert on a capable
  Linux runner that `HwTrace.available(SINGLESTEP)` is `true` and its resolved path is
  **under the installed package** (not `build/`), and that on macOS the tier cleanly
  self-skips. This is the Track-0 `--where` accessor doing real work.

## Track 5 — Docs + CHANGELOG + source-binding note — **done**

- Update [`docs/packaging.md`](../../../reference/packaging.md) and
  [`docs/native-tracing.md`](../../../guides/tracing/native-tracing.md): the tiers now ship in the six
  payload packages (per the matrix); the four source bindings need the consumer to
  build `shared-drtrace`/`shared-hwtrace` and link/point the env var (document the
  one-liner).
- `make help` line for any new `-full` target; one `CHANGELOG.md` entry per tier landed.

---

## Licensing summary

The packages are **already effectively GPL-2.0** (Unicorn + Keystone are GPL-2.0;
Capstone BSD-3; asm-test MIT). Every dep this plan adds is **permissive** and adds no
new copyleft:

- **DynamoRIO** — BSD-3-Clause (core only; no drmgr/drwrap, so no LGPL-2.1).
- **libipt** — BSD; **OpenCSD** — BSD-3-Clause; **libbpf** — convey under its BSD-2 option.

`collect-licenses.sh` gains one `emit` line per bundled dep (version from the build), and
the existing GPL "written offer" is unaffected.

## Scope / non-goals

- **No API or behavior change.** `available()` still self-gates on hardware/OS. Bundling
  only removes the manual `make shared-*` / `DYNAMORIO_HOME` step; it never makes a tier
  claim to run where it can't.
- **No new platforms.** drtrace stays Linux-x86-64; hwtrace stays Linux. macOS/Windows
  packages self-skip both, as today.
- **Not** bundling into the source-dist bindings (Rust/Zig/C++/Go) — they have no
  payload; Track 5 documents their build/link path instead.
- **Not** shipping the eBPF `libbpf` detector by default (it needs `CAP_BPF` and self-
  skips) — it rides only the optional "full" hwtrace lane.

## Verification

1. **Track 1:** `make package-libs hwtrace` on Linux stages `libasmtest_hwtrace.so`; a
   fresh `pip install`/gem/npm install (with `$ASMTEST_HWTRACE_LIB` unset) reports
   `HwTrace.available(SINGLESTEP) == true` and a trace with the resolved path **under
   the package**; a `--where`-style assert fails if it resolves from `build/`.
2. **Track 3:** same for `NativeTrace.available()` on `linux-x86_64` with no
   `DYNAMORIO_HOME`; the bundled `libdynamorio.so` resolves via `$ORIGIN`.
3. **Cross-platform honesty:** the macOS and `linux-arm64` (drtrace) packages install
   and **self-skip** the absent tier with a clean message, no crash.
4. **`package-libs-verify`:** passes for a correct merged tree; fails a tier lib with a
   leaked absolute rpath or a missing license note, and fails a darwin slot that wrongly
   carries a Linux-only tier lib.
5. **Size:** record the payload delta per package (esp. the DynamoRIO slot) so the
   Track-3 default-vs-extra decision is made with the real number.
