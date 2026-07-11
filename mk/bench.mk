# mk/bench.mk — cross-system performance & feature benchmarking.
#
# Two producers feed one normalized per-system report:
#   - emu-bench    deterministic instruction / basic-block counts per guest ISA
#                  (host-independent; the cross-arch performance metric)   Phase 2
#   - asmfeatures  a live capability + TRACE-COMPLETENESS sweep of this system
#                  (the trace-parity matrices instantiated live)           Phase 1
# plus the existing native BENCH tier (test_bench --bench, real cyc/ticks). The
# `bench-report` runner merges all three with a system descriptor; `bench-record`
# persists the result into the benchmarks/ tree (golden + per-box history).
# See docs/internal/plans/cross-arch-benchmarking-plan.md. Included AFTER
# native-trace.mk so HWTRACE_OBJS is defined.

# --- emu-bench: deterministic cross-ISA instruction/block counts -----------
$(BUILD)/emu_bench.o: tools/emu_bench.c tools/asmbench_fixtures.h \
                      include/asmtest_emu.h include/asmtest_trace.h | $(BUILD)
	$(CC) $(CFLAGS) $(UNICORN_CFLAGS) -Itools -c $< -o $@

$(BUILD)/emu-bench: $(BUILD)/emu_bench.o $(BUILD)/emu.o $(BUILD)/trace.o \
                    $(BUILD)/disasm.o
	$(CC) $(CFLAGS) $^ $(UNICORN_LIBS) $(CAPSTONE_LIBS) -o $@

.PHONY: emu-bench
emu-bench: $(BUILD)/emu-bench
	./$(BUILD)/emu-bench

# --- asmfeatures: capability + trace-completeness probe --------------------
# Links the full native-trace object set (HWTRACE_OBJS gives availability +
# asmtest_trace_call_auto for host-native completeness), the emulator (emu.o, for
# guest completeness), and the DynamoRIO app object (asmtest_dr_available). The
# optional decoder libs (libipt/OpenCSD/libbpf) are empty on a bare host, so their
# backends self-skip — the probe records that as data, never an error.
$(BUILD)/asmfeatures.o: tools/asmfeatures.c tools/asmbench_fixtures.h \
                        include/asmtest_trace_auto.h include/asmtest_hwtrace.h \
                        include/asmtest_drtrace.h include/asmtest_emu.h \
                        include/asmtest_trace.h | $(BUILD)
	$(CC) $(CFLAGS) $(UNICORN_CFLAGS) -Itools -c $< -o $@

# $(DRAPP_KS_OBJ)/$(DRAPP_KS_LIBS) are the Keystone assembler object+lib, which are
# non-empty ONLY when Keystone is installed — matching how drtrace_app.o is compiled
# (with -DASMTEST_HAVE_KEYSTONE iff Keystone is present), so the assembler reference
# is present-or-absent consistently and a bare host links no assemble.o.
$(BUILD)/asmfeatures: $(BUILD)/asmfeatures.o $(HWTRACE_OBJS) $(BUILD)/emu.o \
                      $(BUILD)/drtrace_app.o $(DRAPP_KS_OBJ)
	$(CC) $(CFLAGS) -rdynamic $^ $(UNICORN_LIBS) $(CAPSTONE_LIBS) \
	    $(LIBIPT_LIBS) $(OPENCSD_LIBS) $(LINK_LIBBPF) $(DRAPP_KS_LIBS) \
	    -ldl -lpthread -o $@

.PHONY: features
features: $(BUILD)/asmfeatures
	./$(BUILD)/asmfeatures

# --- bench-report: one merged, self-describing per-system JSON report -------
BENCH_PRODUCERS := $(BUILD)/test_bench $(BUILD)/emu-bench $(BUILD)/asmfeatures

.PHONY: bench-report
bench-report: $(BENCH_PRODUCERS)
	BUILD=$(BUILD) scripts/bench-report.sh

# --- bench-record: persist this box's report into the benchmarks/ tree ------
.PHONY: bench-record
bench-record: $(BENCH_PRODUCERS)
	BUILD=$(BUILD) scripts/bench-report.sh --record

# --- bench-check: gate the deterministic golden emu counts -----------------
# The emu-bench counts are host/OS-independent, so the committed golden file must
# match a fresh run on every leg; drift means the code changed (regenerate with
# `make bench-record`). Mirrors the manifest / conformance golden gates.
.PHONY: bench-check
bench-check: $(BUILD)/emu-bench
	@$(BUILD)/emu-bench --format=json >$(BUILD)/emu-insns.fresh.json
	@python3 scripts/bench-golden-check.py benchmarks/golden/emu-insns.json \
	    $(BUILD)/emu-insns.fresh.json

# --- bench-compare: aggregate report(s) into a cross-system matrix ----------
.PHONY: bench-compare
bench-compare:
	@python3 scripts/bench-compare $(BENCH_REPORTS)

# --- docker-bench: reproduce a CI leg's report in the container -------------
# DOCKER_PLATFORM=linux/arm64 emulates the aarch64 leg (real cycles are marked
# virtualized there; the emulated counts + feature probe stay valid).
.PHONY: docker-bench
docker-bench: docker-build
	$(_docker_run) make bench-report
