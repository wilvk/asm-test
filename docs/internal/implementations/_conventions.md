# Repo conventions the implementation docs rely on

Read this once. The individual implementation documents assume these rules
rather than repeating them. Everything here is verifiable from the repo root
([CLAUDE.md](../../../CLAUDE.md), [CONTRIBUTING.md](../../../CONTRIBUTING.md),
[Makefile](../../../Makefile)).

## Build & test entry points

`make help` is the source of truth for targets. The ones the docs use most:

- `make test` — build + run the example suites (default target).
- `make check` — framework self-tests (`tests/expect.sh`): assertions, runner,
  crash/hang containment.
- `make fmt` / `make fmt-check` — clang-format (CI-gated). **Canonical style is
  clang-format 18** (CI pins it). The Makefile's `CLANG_FORMAT ?= clang-format`
  is unpinned, so a newer host clang-format flags a clean tree — use
  `make docker-fmt-check` / `make docker-fmt`, or
  `make fmt CLANG_FORMAT=clang-format-18`.
- **Docker CI lanes** reproduce the Linux half of CI in a container and are
  preferred over host installs (see the dependency rule below):
  `make docker-test`, `make docker-ci`, `make docker-drtrace`,
  `make docker-hwtrace*`, `make docker-dataflow*`, `make docker-bindings`,
  `make docker-<lang>`, `make docker-fmt`, `make docker-docs`. Pass
  `DOCKER_PLATFORM=linux/arm64` for the aarch64 lane.
- Tier lanes (`drtrace-test`, `hwtrace-test`, `ibs-test`, `dataflow-test`,
  `hwtrace-bindings-test`, …) **self-skip with a printed reason** when a real
  gate (hardware / credentials) is absent.

## The dependency rule (CLAUDE.md — this overrides "just skip it")

A **missing installable dependency is never a blocker.** Add it where the work
runs — the relevant `Dockerfile.*` and its `docker-*` rule, not the host — and
**pin the version**, following the pattern already in the tree:

- prebuilt release tarball → `ARG <TOOL>_VERSION` + `curl` + a SHA-256 line in
  [scripts/third-party-digests.txt](../../../scripts/third-party-digests.txt) +
  vendor the license under [licenses/](../../../licenses/). The DynamoRIO block
  in [Dockerfile.drtrace](../../../Dockerfile.drtrace) /
  [scripts/fetch-dynamorio.sh](../../../scripts/fetch-dynamorio.sh) is the
  reference; the Pin/SDE docs mirror it exactly.
- built from pinned source → a `scripts/build-*.sh` (e.g. Capstone 5.0.1).
- fetched headers → a pinned sparse-checkout tag (see
  [mk/native-trace.mk](../../../mk/native-trace.mk)).
- apt, where a pinned distro package suffices.

**A test that can only ever self-skip is not a test.** If a lane lacks what a
feature needs *in order to be tested*, extend that image — do not leave a
self-skipping lane behind.

The **only** legitimate self-skip gates are what cannot be installed:
**hardware** (a specific CPU generation, Intel PT, AMD Zen 3/4/5, ARM
CoreSight, Apple silicon, `/dev/kvm`) and **credentials**. Record those and
self-skip with a reason.

## Makefile layout

Core variables, knobs, and native build/test rules live in the top-level
[Makefile](../../../Makefile). Large target groups are split into
[mk/](../../../mk/) by concern and `include`d in place (so they share every
variable): `mk/docker.mk`, `mk/native-trace.mk` (DynamoRIO + Intel PT /
CoreSight), `mk/dataflow.mk`, `mk/bindings.mk`, `mk/win64.mk`, `mk/bench.mk`,
`mk/cli.mk`, `mk/docs.mk`. **Edit a target where it lives**; edit shared
variables/knobs in the parent Makefile.

## The shared trace sink

Any trace-producing work fills [asmtest_trace_t](../../../include/asmtest_trace.h)
(offsets from the region base; append/dedup/`truncated` discipline) so its output
is diffable against every existing backend. Cross-validation against another
backend is usually the point.

## Docs & changelog

- User-facing docs live under [docs/](../../../docs/) (Sphinx; `make docs` /
  `make docker-docs`; the build runs `-W`, fail-on-warning).
- **`docs/internal/**` is excluded from the published site.** Internal docs
  (including these) may use ordinary relative links to anywhere in the repo;
  they render on GitHub. Published pages may link *into* `docs/internal` only
  via GitHub blob URLs (a relative xref into an excluded file warns and breaks
  the `-W` build).
- User-visible changes append an entry under `## [Unreleased]` in
  [CHANGELOG.md](../../../CHANGELOG.md) (one `Added` / `Changed` / `Fixed`
  header each).

## Where these docs live in the internal tree

Per [../README.md](../README.md): `plans/` holds active plans, `analysis/` holds
investigation notes, `archive/` holds completed material. This
`implementations/` directory is the actioned, implementation-ready form of the
open items in those plans and notes. When a document here is fully implemented,
the matching plan/analysis item should be struck and, when its whole plan is
done, that plan moves to `archive/plans/` in the same change.
