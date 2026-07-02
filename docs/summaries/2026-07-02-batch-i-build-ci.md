# Implementation summary — Batch I: build / CI / packaging (findings #40–48, repo-review B1/B3/B4/B6)

*Source:* [2026-07-02 code review](../analysis/2026-07-02-code-review.md) #40–48;
[2026-07-02 repo review](../reviews/2026-07-02-repo-review.md) B1, B3, B4, B6.
*Validated:* full clean `make test check usecases` green (5/8/2/4/2 suites, check
36/36, usecases 10/10); YAML parses; `make install` now stages the assembler
header; the bpf fallback header compiles `codeimage.bpf.c` clean (Docker clang);
the new version-consistency check passes and catches injected drift.

## Code-review findings

- **#40 (High)** `release.yml` corresponding-source: added job-scoped
  `permissions: contents: write` and a `gh release view || gh release create
  --verify-tag` before `gh release upload`, so the GPL source attaches on a real
  tag instead of 403/"release not found".
- **#41 (Med)** `mk/bindings.mk` lua packer now copies
  `asmtest-$(ASMTEST_VERSION)-1.rockspec` (was the literal `1.0.0` name that
  `sync-version` renames on the first bump).
- **#42 (Med)** `Makefile` — added `PLATFORM_HDRS` (platform.h + glob_match.h +
  platform_win32.h) to the `asmtest.o` rules and `TIER_HDRS` (`$(wildcard
  include/asmtest_*.h)`) to the example/test object pattern rules; `mk/bindings.mk`
  `asmtest_nomain.o` likewise. Verified a `platform.h`/`asmtest_emu.h` edit now
  rebuilds the dependent objects.
- **#43 (Med)** added `include/asmtest_trace.h` to the pic `emu.o`/`ffi.o`/`fuzz.o`
  rules and `include/asmtest_codeimage.h` to both `ptrace_backend.o` rules, matching
  their non-PIC twins (no more mixed struct layouts in a relinked `.so`).
- **#44 (Med)** `ci.yml` bindings-parity job runs `make check-version`.
- **#45 (Low)** the `[ Linux ] && apt-get … || true` blocks in `ci.yml` and
  `release.yml` (×2) are now `if [ Linux ]; then apt-get …; fi`, so a real Linux
  apt failure fails the step instead of silently shipping decoder-less hwtrace.
- **#46 (Low)** `make install`/`uninstall` now include `include/asmtest_assemble.h`
  (the `asmtest-emu.pc` superset advertised the assembler tier without its header).
- **#47 (Low)** `release.yml native-all` installs `llvm` before
  `package-libs-verify`, so the darwin Mach-O checks run instead of self-skipping.
- **#48 (Med)** `bpf/vmlinux_min.h` gained `enum bpf_map_type` (UAPI values), the
  `BPF_ANY`/update-flag enum, `struct bpf_pidns_info`, and the `__be16/__be32/__wsum`
  aliases, so the BTF-less fallback build of `codeimage.bpf.c` compiles (was 13
  errors); header comment corrected. Verified with `clang -target bpf` in Docker.

## Repo-review B-series

- **B1 (Med–High)** folded the active build knobs into core-object identity via a
  `$(BUILD)/.build-flags` sentinel (mirroring `mk/native-trace.mk`'s `.drapp-flags`),
  wired into `asmtest.o`, `capture.o` (GAS+NASM), the example/test object pattern
  rules, and the emu-tier non-PIC objects (`emu/ffi/disasm/fuzz/trace`). Verified
  `make test && make SAN=1 test` now recompiles (was a no-op → false-green sanitizer),
  and flips back on `ASM_SYNTAX`/knob change.
- **B3 + B4 (Med / Med–Low)** added `scripts/check-thirdparty-versions.sh` asserting
  DynamoRIO (6 sites), Keystone, and Capstone versions agree across the Make layer,
  CI, the Dockerfiles, and the GPL corresponding-source script; wired into the
  `ci.yml` bindings-parity job. Verified it passes today and fails on injected drift.
- **B6 (Med, CI security)** moved `PYPI_TOKEN`, the NPM/RubyGems/NuGet tokens, and
  `CARGO_REGISTRY_TOKEN` out of job-level `env` into their single publish step, so
  build/smoke steps that run the just-built package can't read them; publish steps
  gate on the tag and skip gracefully when the secret is unset.

## Deferred (documented, not done)

- **B2** (no `darwin-x86_64` payload in published packages) — a release-matrix
  policy decision: add a `macos-13` leg to `release.yml` (with cost implications)
  or document macOS packages as arm64-only. Left to the maintainer.
- **B5** (no checksum/signature pin on fetched/built third-party binaries) — needs a
  trusted SHA-256/signature anchor recorded per artifact (a maintainer trust
  decision); the machinery to verify is a follow-up.
- **B7** (recursive-make `-j` races in `*-bindings-test`) — Low; the safe fix
  restructures the per-language recursive `$(MAKE) shared-*` pattern (which passes
  `DRAPP_KEYSTONE=0`) into a shared once-built prerequisite, and needs `-j` race
  validation. Deferred to avoid destabilizing the drapp build with no way to
  validate the race here.
