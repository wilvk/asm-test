# Troubleshooting & FAQ

asm-test's core (capture, assertions, the runner) needs only a C compiler, but
the optional tiers are environment-sensitive — they **self-skip** where their
library, hardware, or privilege is missing rather than fail. This page maps the
common symptoms to fixes; most "it skipped" cases are by design.

## Build & consume

**`fatal error: asmtest.h: No such file` / linker can't find `-lasmtest`.**
Pass the include and lib dirs from the install prefix, e.g. `cc -I$PREFIX/include
… -L$PREFIX/lib -lasmtest`, or use pkg-config: `` cc `pkg-config --cflags --libs
asmtest` … ``. Install with `make install` (static lib + headers) and, for the
shared tiers, `make install-shared` / `install-shared-emu` /
`install-shared-hwtrace` / `install-shared-drtrace`.

**Compiling as strict `-std=c11` used to fail.** The public header is pure ISO C
and includes cleanly under `-std=c11`. Building the *library* itself strict needs
POSIX symbols (fork, `sigaltstack`, `CLOCK_MONOTONIC`); the Makefile passes
`-D_DEFAULT_SOURCE` so `make CSTD=c11` works.

**g++ error "taking address of temporary array" from the `ASM_CALL*` macros.**
Fixed — the capture macros build a local array, so they compile from C and C++.
Update to a current header.

## Language bindings

**A binding can't find the native library at run time / `image not found`.**
The dynamic-language bindings dlopen the prebuilt `libasmtest_emu`; point the
loader at it. Set `ASMTEST_LIB=/path/to/libasmtest_emu.so` (the exact file), and
add its directory to `LD_LIBRARY_PATH` (Linux) or `DYLD_LIBRARY_PATH` (macOS).
`make <lang>-test` sets these for you against the local `build/` tree.

**`asm_available()` / `disas_available()` is false in a binding.** The loaded lib
lacks Keystone/Capstone. Use `libasmtest_emu` (the superset), which carries both;
`ASMTEST_LIB` pointing at an older/leaner lib makes the in-line assembler and
disassembler self-skip.

**Go: `cannot find -lasmtest_corpus`.** That fixture is test-only; build the
conformance suite with `-tags asmtest_corpus`. A consumer binary links only
`libasmtest_emu`.

**Java: the jar won't load (`UnsupportedClassVersionError` / preview).** The
binding targets `--release 22` (FFM is final since JDK 22) with no preview flags —
run it on JDK 22 or newer.

## In-line assembler (Keystone) & disassembler (Capstone)

**`make asm-test` / the assembler tier is unavailable.** Keystone *and* Capstone
have no distro package, so they are pinned source builds: run
`./scripts/build-keystone.sh` and `./scripts/build-capstone.sh`. `make deps
DEPS_ARGS=--asm` installs the *packaged* deps (nasm, pkg-config, libunicorn,
patchelf) and, for Keystone/Capstone, only **prints a pointer** to those two
scripts — on every platform it does not build them itself, so run the scripts
directly. The build verifies the pinned commit against
`scripts/third-party-digests.txt`.

## Emulator tier (Unicorn)

**`make emu-test` fails to link (`-lunicorn`).** Install libunicorn with `make
deps DEPS_ARGS=--emu` (unicorn comes from your package manager; **Capstone is a
pinned source build** — run `./scripts/build-capstone.sh`, which `make deps` only
points you to). The emulator tier is optional; the core suites don't need it.

## Hardware-trace tiers (Intel PT / AMD LBR / ARM CoreSight / single-step)

**`# SKIP hwtrace … capture`.** Expected off the matching hardware, without the
decoder library, or without perf permission. The `hwtrace-test` C job validates
*decode* everywhere; live *capture* self-skips. Check the reason with
`asmtest_hwtrace_skip_reason`.

**perf capture is denied even on the right CPU.** The PMU-based backends need
`perf_event_paranoid` low enough (and, in a container, `--cap-add=PERFMON`).
Lower it with `sudo sysctl kernel.perf_event_paranoid=1` (or `-1`). The
**single-step** backend needs no PMU/perf/privilege — it runs on any x86-64 Linux
**or macOS** host and is what the language-binding wrappers use.

## DynamoRIO in-process tracing

**`# SKIP: DynamoRIO not found`.** Set `DYNAMORIO_HOME=/path/to/DynamoRIO-Linux-…`
(or `ASMTEST_DR_LIB`, or bundle it), then `make shared-drtrace drtrace-client`.
`asmtest_dr_skip_reason` explains a `0` from `asmtest_dr_available()`. Note: a
`libdynamorio.so` reachable only via `ldconfig`/`LD_LIBRARY_PATH` is reported
unavailable (the probe is side-effect-free) yet may still load at init.

## eBPF code-image detector

**codeimage self-skips.** The detector needs `CAP_BPF` (and a recent kernel with
BTF). Run with the capability (`--cap-add=BPF --cap-add=PERFMON` in a container),
or accept the skip — it degrades cleanly.

## Docker / arm64

**Reproducing the arm64 CI lane locally.** Pass `DOCKER_PLATFORM=linux/arm64`; on
Linux enable binfmt once with `docker run --privileged tonistiigi/binfmt`. arm64
CI runs the `test`, `emu`, `asm`, and `package-libs` jobs (NASM is x86-64 only).
Note: `qemu-user` can't single-step, so the arm64 lane skips the ptrace/single-step
hardware-trace paths.

## FAQ

**A test "passed" but ran nothing.** A typo'd `--filter` selects zero tests; the
runner warns to stderr and prints `1..0`. Use `--fail-if-no-tests` to make an
empty selection exit nonzero.

**Colors show in CI logs, or don't show when I want them.** Color is `auto` by
default (a TTY, `NO_COLOR` unset). Force it with `--color=always`, disable with
`--color=never` or `NO_COLOR=1`.

**How do I get machine-readable results?** `--format=junit` for test results
(with `<failure>`/`<error>`/`<system-out>`), and `--bench-format=json` for
benchmarks (min/median/mean + stddev + cv).
