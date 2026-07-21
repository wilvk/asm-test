# fuzz.mk — external-engine (libFuzzer / AFL++) coverage shim (Track E,
# deliverable 3). Feeds the emulator's basic-block coverage into an industrial
# fuzzer's feedback channel WITHOUT compiler-instrumenting the guest bytes: they
# run under Unicorn, invisible to clang/afl, so each harness registers an
# external coverage map and writes the emulator's executed block offsets into it
# through the tested emu_cover_hits seam (src/fuzz.c). This is the exact
# mechanism afl-qemu-trace / Unicorn-mode / FRIDA-mode use for binary-only
# targets. See docs/guides/fuzzing-shim.md.
#
# All variables/knobs (CC, CFLAGS, BUILD, FRAMEWORK_OBJS, UNICORN_*, CAPSTONE_*)
# come from the parent Makefile, which reads this file in place. clang + afl++
# are apt-installable, so per CLAUDE.md they are added to the lane where the work
# runs (Dockerfile.fuzz + docker-fuzz, mk/docker.mk), NOT gated away:
# `make docker-fuzz` builds that image and runs `make fuzz-shim-test`.

CLANG          ?= clang
AFL_CLANG_FAST ?= afl-clang-fast

# A main()-less build of the framework runtime: libFuzzer and AFL each supply
# their OWN main(), so the harnesses cannot link the ordinary asmtest.o (it
# carries the test-runner main — a multiple-definition clash). asmtest.c documents
# -DASMTEST_NO_MAIN for exactly this ("omit it and supply your own"); the object
# still exports the asmtest_rng_* symbols src/fuzz.c references.
# The ONE $(BUILD)/asmtest_nomain.o recipe lives in mk/bindings.mk (the
# conformance runner links the same object) — a second recipe here made GNU
# make warn "overriding recipe" on every run and silently dropped this file's
# .build-flags prerequisite (bindings.mk, included later, won).

# The emulator object set `emu-test` links, minus the test file AND with the
# main-less framework object. Built by $(CC) via the parent Makefile's rules; the
# harnesses are compiled + linked against them by clang / afl-clang-fast.
FUZZ_ENGINE_OBJS := $(BUILD)/asmtest_nomain.o $(BUILD)/capture.o $(BUILD)/emu.o \
                    $(BUILD)/trace.o $(BUILD)/disasm.o $(BUILD)/fuzz.o
FUZZ_ENGINE_LIBS := $(UNICORN_LIBS) $(CAPSTONE_LIBS)
FUZZ_ENGINE_CFLAGS := -Iinclude $(UNICORN_CFLAGS) $(CAPSTONE_CFLAGS) $(CAPSTONE_DEF)

# --- libFuzzer harnesses (T2) ----------------------------------------------
# `clang -fsanitize=fuzzer` links libFuzzer's own main() AND its SanitizerCoverage
# runtime; the guest is data (Unicorn runs it), so the harness registers an
# EXTERNAL 8-bit counter array that libFuzzer consumes as compiler-generated. Two
# variants: the fault-free CLASSIFY3 baseline, and the crashing guest whose
# negative path dereferences [rdi] (-DFUZZ_CRASH_GUEST).
$(BUILD)/fuzz_libfuzzer: examples/fuzz_libfuzzer.c $(FUZZ_ENGINE_OBJS) | $(BUILD)
	$(CLANG) -fsanitize=fuzzer $(FUZZ_ENGINE_CFLAGS) $< $(FUZZ_ENGINE_OBJS) \
	  $(FUZZ_ENGINE_LIBS) -o $@

$(BUILD)/fuzz_libfuzzer_crash: examples/fuzz_libfuzzer.c $(FUZZ_ENGINE_OBJS) | $(BUILD)
	$(CLANG) -fsanitize=fuzzer -DFUZZ_CRASH_GUEST $(FUZZ_ENGINE_CFLAGS) $< \
	  $(FUZZ_ENGINE_OBJS) $(FUZZ_ENGINE_LIBS) -o $@

.PHONY: fuzz-libfuzzer
fuzz-libfuzzer: $(BUILD)/fuzz_libfuzzer $(BUILD)/fuzz_libfuzzer_crash
	@echo "built $(BUILD)/fuzz_libfuzzer (+_crash) via clang -fsanitize=fuzzer"

# --- AFL++ harnesses (T3) ---------------------------------------------------
# afl-clang-fast instruments only the harness's OWN code (a small constant
# background in the map); the guest runs under Unicorn, invisible to it, so each
# executed guest block is written into AFL's shared-memory bitmap by hand via
# asmtest_afl_map_bump. That helper lives in examples/fuzz_afl_map.c, built by
# $(CC) (NOT afl-clang-fast) — the afl instrumentation pass rewrites a harness's
# own __afl_area_ptr reference to an undefined module-local __afl_area_ptr.2, so
# the map write must sit in a plain-compiled TU. Both engines share it. AFL++
# ships no SanitizerCoverage runtime, so this is the ONLY way to feed AFL the
# guest coverage (a verbatim reuse of the libFuzzer sancov harness would not
# link under AFL).
$(BUILD)/fuzz_afl_map.o: examples/fuzz_afl_map.c $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# Path B — native persistent-mode forkserver (examples/fuzz_afl.c), no libFuzzer.
$(BUILD)/fuzz_afl: examples/fuzz_afl.c $(FUZZ_ENGINE_OBJS) $(BUILD)/fuzz_afl_map.o | $(BUILD)
	$(AFL_CLANG_FAST) $(FUZZ_ENGINE_CFLAGS) $< $(FUZZ_ENGINE_OBJS) \
	  $(BUILD)/fuzz_afl_map.o $(FUZZ_ENGINE_LIBS) -o $@

$(BUILD)/fuzz_afl_crash: examples/fuzz_afl.c $(FUZZ_ENGINE_OBJS) $(BUILD)/fuzz_afl_map.o | $(BUILD)
	$(AFL_CLANG_FAST) -DFUZZ_CRASH_GUEST $(FUZZ_ENGINE_CFLAGS) $< \
	  $(FUZZ_ENGINE_OBJS) $(BUILD)/fuzz_afl_map.o $(FUZZ_ENGINE_LIBS) -o $@

.PHONY: fuzz-afl
fuzz-afl: $(BUILD)/fuzz_afl $(BUILD)/fuzz_afl_crash
	@echo "built $(BUILD)/fuzz_afl (+_crash) via afl-clang-fast (native forkserver)"

# Path A — reuse the libFuzzer harness under aflpp_driver: afl-clang-fast
# -fsanitize=fuzzer links libAFLDriver.a (its own main + forkserver loop) in
# place of libFuzzer. -DFUZZ_AFL_DRIVER swaps the harness's coverage sink from
# sancov counters to AFL's map (the same fuzz_afl_map.o helper).
$(BUILD)/fuzz_afl_driver: examples/fuzz_libfuzzer.c $(FUZZ_ENGINE_OBJS) $(BUILD)/fuzz_afl_map.o | $(BUILD)
	$(AFL_CLANG_FAST) -fsanitize=fuzzer -DFUZZ_AFL_DRIVER $(FUZZ_ENGINE_CFLAGS) \
	  $< $(FUZZ_ENGINE_OBJS) $(BUILD)/fuzz_afl_map.o $(FUZZ_ENGINE_LIBS) -o $@

$(BUILD)/fuzz_afl_driver_crash: examples/fuzz_libfuzzer.c $(FUZZ_ENGINE_OBJS) $(BUILD)/fuzz_afl_map.o | $(BUILD)
	$(AFL_CLANG_FAST) -fsanitize=fuzzer -DFUZZ_AFL_DRIVER -DFUZZ_CRASH_GUEST \
	  $(FUZZ_ENGINE_CFLAGS) $< $(FUZZ_ENGINE_OBJS) $(BUILD)/fuzz_afl_map.o \
	  $(FUZZ_ENGINE_LIBS) -o $@

.PHONY: fuzz-afl-driver
fuzz-afl-driver: $(BUILD)/fuzz_afl_driver $(BUILD)/fuzz_afl_driver_crash
	@echo "built $(BUILD)/fuzz_afl_driver (+_crash) via afl-clang-fast -fsanitize=fuzzer (aflpp_driver)"

# --- CI smoke: prove BOTH engines find the planted crash (T4) ---------------
# The one command `make docker-fuzz` runs: build every harness, then a bounded
# libFuzzer baseline (no crash) + crash-finding run, the AFL native single-input
# replay, and a short `afl-fuzz -V` on the crashing guest. Each engine MUST find
# its planted crash — a non-zero exit with a "FAIL:" line otherwise. This is a
# real test, never a self-skip: clang + afl++ are installed in Dockerfile.fuzz.
FUZZ_LIBFUZZER_RUNS ?= 50000
FUZZ_CRASH_RUNS     ?= 200000
FUZZ_AFL_SECONDS    ?= 15

.PHONY: fuzz-shim-test
fuzz-shim-test: fuzz-libfuzzer fuzz-afl fuzz-afl-driver
	@echo "== fuzz-shim-test: libFuzzer + AFL++ external-engine coverage shim =="
	@set -e; \
	work="$$(mktemp -d)"; trap 'rm -rf "$$work"' EXIT; \
	echo "-- libFuzzer baseline (CLASSIFY3, no crash expected) --"; \
	$(BUILD)/fuzz_libfuzzer -runs=$(FUZZ_LIBFUZZER_RUNS) -max_len=8 \
	  -artifact_prefix=$$work/ >$$work/lf.log 2>&1; \
	echo "   exit 0, $$(grep -oE 'cov: [0-9]+ ft: [0-9]+' $$work/lf.log | tail -1) (guest paths grew coverage)"; \
	echo "-- libFuzzer crash-finding (planted fault on the negative path) --"; \
	if $(BUILD)/fuzz_libfuzzer_crash -runs=$(FUZZ_CRASH_RUNS) -max_len=8 \
	     -artifact_prefix=$$work/ >$$work/lfc.log 2>&1; then \
	  echo "FAIL: libFuzzer crash harness exited 0 — coverage feedback did not reach the fault"; \
	  exit 1; fi; \
	ls $$work/crash-* >/dev/null 2>&1 || { \
	  echo "FAIL: libFuzzer produced no crash artifact"; tail -20 $$work/lfc.log; exit 1; }; \
	echo "   libFuzzer found the crash: $$(basename $$(ls $$work/crash-* | head -1))"; \
	echo "-- AFL native single-input replay (deterministic, no afl-fuzz) --"; \
	printf '\371\377\377\377' | $(BUILD)/fuzz_afl >/dev/null 2>&1; \
	echo "   AFL replay exit 0"; \
	echo "-- AFL crash-finding under afl-fuzz -V $(FUZZ_AFL_SECONDS) --"; \
	mkdir -p $$work/seeds; printf '\005\000\000\000' > $$work/seeds/a; \
	AFL_SKIP_CPUFREQ=1 AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES=1 \
	  afl-fuzz -i $$work/seeds -o $$work/out -V $(FUZZ_AFL_SECONDS) \
	    -- $(BUILD)/fuzz_afl_crash >$$work/afl.log 2>&1 || true; \
	n=$$(ls $$work/out/default/crashes/ 2>/dev/null | grep -c '^id:' || true); \
	[ "$$n" -ge 1 ] || { \
	  echo "FAIL: afl-fuzz found no crash in $(FUZZ_AFL_SECONDS)s — coverage feedback broke"; \
	  tail -25 $$work/afl.log; exit 1; }; \
	echo "   afl-fuzz found $$n crash(es) via the emulator-coverage forkserver"; \
	echo "== fuzz-shim-test PASS: both engines steered to the planted crash =="
