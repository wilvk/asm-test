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
$(BUILD)/asmtest_nomain.o: src/asmtest.c include/asmtest.h $(PLATFORM_HDRS) $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) -DASMTEST_NO_MAIN -c $< -o $@

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
