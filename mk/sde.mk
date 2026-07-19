# sde.mk — Intel SDE future/absent-ISA test lane.
#
# Included by ../Makefile (split out by concern). All variables/knobs (BUILD,
# CFLAGS, CC, SUITES, ...) come from the parent Makefile, which reads this file in
# place; edit targets here, knobs there.
#
# `make sde-test` runs the suite under `sde64 -future`, which emulates ISA the
# host CPU lacks (APX r16-r31 / REX2, AVX10.2, AMX, AVX-512-on-AVX2) for the WHOLE
# process. That gives future-ISA routines the full register/flag/memory/ABI
# assertion battery on ANY x86-64 host — untestable any other way here (the
# DynamoRIO tier runs on real silicon; the Unicorn tier's QEMU 5.0.1 predates AVX
# TCG). The lane fetches a pinned SDE, proves it is byte-for-byte transparent to
# correct baseline code (the null test), and adds the APX fixtures.
#
# Set SDE_HOME=$(scripts/fetch-sde.sh) to run on a host, or use `make docker-sde`
# (the image carries a pinned, digest-gated SDE + APX-capable GAS + NASM).

# Gate order mirrors mk/cli.mk: ARCHITECTURE first (SDE is x86-only — an
# un-installable gate), THEN the installable dependency (the SDE kit). Off x86-64
# there is nothing to install, so that path is a REAL self-skip; on x86-64 without
# the kit the skip points at the fetch script (per CLAUDE.md the lane exists so
# this only fires on a host that chose not to fetch).
SDE_ARCH := $(shell uname -m)
SDE_HOME ?=
SDE64    := $(SDE_HOME)/sde64
ifneq ($(wildcard $(SDE64)),)
SDE_AVAILABLE := 1
endif
# The chip/CPUID selector. `-future` = XED chip FUTURE (a strict superset of
# DIAMOND_RAPIDS): every APX_F*/AVX10_2* ISA set, plus the chip-check that errors
# on any instruction illegal for that chip. Override to pin a narrower chip.
SDE_CHIP ?= -future

.PHONY: sde-test
ifneq ($(SDE_ARCH),x86_64)
sde-test:
	@echo "== sde-test =="
	@echo "# SKIP: Intel SDE is x86-only; host is $(SDE_ARCH)"
	@echo "1..0 # skipped"
else ifndef SDE_AVAILABLE
sde-test:
	@echo "== sde-test =="
	@echo "# SKIP: Intel SDE not found. Set SDE_HOME=\$$(scripts/fetch-sde.sh)"
	@echo "1..0 # skipped"
else
# Live lane. Each sub-target is chained with the drtrace-test idiom so later tasks
# (the APX suite, the AVX-512 un-skip, the Unicorn cross-check) each add one line.
sde-test:
	@$(MAKE) --no-print-directory sde-null-test
	@$(MAKE) --no-print-directory sde-apx-test
	@echo "== sde-test: all SDE sub-lanes passed =="
endif

# --- Transparency null test -------------------------------------------------
# On suites with NO CPUID-dependent skips, SDE must be byte-for-byte transparent:
# the native TAP and the `sde64 -future` TAP are identical. TAP carries no timings
# (verified), so `diff` is exact — any difference IS the transparency-violation
# report. test_simd is deliberately absent: its capability skips SHOULD differ
# under SDE, which is the AVX-512 un-skip assertion's job (sde-avx512-test).
.PHONY: sde-null-test
sde-null-test: $(BUILD)/test_arith $(BUILD)/test_capture $(BUILD)/test_mem
	@set -e; for t in test_arith test_capture test_mem; do \
	  echo "== sde-null: $$t =="; \
	  $(BUILD)/$$t                        > $(BUILD)/sde-null-$$t.native.tap; \
	  $(SDE64) $(SDE_CHIP) -- $(BUILD)/$$t > $(BUILD)/sde-null-$$t.sde.tap; \
	  diff -u $(BUILD)/sde-null-$$t.native.tap $(BUILD)/sde-null-$$t.sde.tap; \
	done; echo "sde-null-test: native and sde64 $(SDE_CHIP) TAP identical"

# --- APX (r16-r31 / REX2 / NDD) fixture suite -------------------------------
# The APX routines in examples/apx_basic.s #UD on all shipping silicon, so they
# run ONLY under SDE. test_apx_basic is in SUITE_EXCLUDES so `make test` never has
# to assemble it; a host-side sde-test degrades honestly when the host assembler
# predates APX (older binutils) — probe the compiler's assembler for an EGPR move
# + NDD add first. The pinned Dockerfile.sde carries binutils 2.46.1, so the probe
# always passes in-container.
SDE_GAS_APX := $(shell printf 'movq %%r16, %%rax\naddq %%rsi, %%rdi, %%rax\n' | \
                 $(CC) -x assembler -c -o /dev/null - 2>/dev/null && echo 1)

.PHONY: sde-apx-test
sde-apx-test:
	@echo "== sde-apx-test =="
ifneq ($(SDE_GAS_APX),1)
	@echo "# SKIP: assembler cannot encode APX (need binutils >= 2.43; make docker-sde runs the pinned 2.46.1)"
	@echo "1..0 # skipped"
else
	@$(MAKE) --no-print-directory $(BUILD)/test_apx_basic
	@$(SDE64) $(SDE_CHIP) -- $(BUILD)/test_apx_basic > $(BUILD)/sde-apx.tap; rc=$$?; \
	 cat $(BUILD)/sde-apx.tap; \
	 [ $$rc -eq 0 ] || { echo "FAIL: test_apx_basic exited $$rc under sde64 $(SDE_CHIP)"; exit 1; }; \
	 if grep -E '^(ok|not ok) [0-9]+ - apx\.' $(BUILD)/sde-apx.tap | grep -q '# SKIP'; then \
	   echo "FAIL: an apx.* case is STILL skipped under $(SDE_CHIP) — APX CPUID gate did not open"; \
	   exit 1; \
	 fi; \
	 echo "sde-apx-test: all apx.* cases ran green under sde64 $(SDE_CHIP) (no CPUID skip)"
endif
