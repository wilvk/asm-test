# dataflow.mk — data-flow tracing tier (docs/internal/plans/data-flow-tracing-plan.md).
#
# Included by ../Makefile (split out by concern). All knobs (CSTD, WERROR,
# ASM_SYNTAX, BUILD, CAPSTONE_*, UNICORN_*, ...) come from the parent Makefile,
# which reads this file in place after they are defined; edit targets here.
#
# Three layers, on separate dependency tiers so the pure spine builds everywhere:
#   dataflow.o          PURE C — the L0 value-trace sink + L1 def-use + L2 slicer
#                       (no Capstone, no Unicorn). Runs on every host.
#   dataflow_operands.o Capstone operand read/write-set enumerator (detail mode).
#                       Degrades to a no-op without Capstone.
#   dataflow_emu.o      Unicorn L0 producer (Phase 2). Built only when libunicorn
#                       is present; the emulator test self-skips otherwise.
#
# `make dataflow-test` builds + runs the three suites. The pure suites run on any
# host; the emulator suite is gated on libunicorn.

# --- objects ---------------------------------------------------------------
$(BUILD)/dataflow.o: src/dataflow.c include/asmtest_valtrace.h \
                     include/asmtest_trace.h $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/dataflow_operands.o: src/dataflow_operands.c include/asmtest_valtrace.h \
                              include/asmtest_trace.h $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) $(CAPSTONE_CFLAGS) $(CAPSTONE_DEF) -c $< -o $@

# Unicorn producer: needs both Unicorn (engine) and Capstone (operand enumerator).
$(BUILD)/dataflow_emu.o: src/dataflow_emu.c include/asmtest_valtrace.h \
                         include/asmtest_trace.h $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) $(UNICORN_CFLAGS) $(CAPSTONE_CFLAGS) $(CAPSTONE_DEF) -c $< -o $@

# --- test-object compile knobs ---------------------------------------------
# The examples/%.c pattern rule (root Makefile) compiles these with plain CFLAGS;
# the Capstone/Unicorn suites need the extra include paths + the -DASMTEST_HAVE_CAPSTONE
# guard so their #ifdef'd assertions compile. test_dataflow.o needs nothing extra.
$(BUILD)/test_operands.o: CFLAGS += $(CAPSTONE_CFLAGS) $(CAPSTONE_DEF)
$(BUILD)/test_dataflow_emu.o: CFLAGS += $(UNICORN_CFLAGS) $(CAPSTONE_CFLAGS) \
                                        $(CAPSTONE_DEF)

# --- test binaries (standalone TAP, like test_ibs; no framework runtime) ----
$(BUILD)/test_dataflow: $(BUILD)/dataflow.o $(BUILD)/test_dataflow.o
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_operands: $(BUILD)/dataflow.o $(BUILD)/dataflow_operands.o \
                        $(BUILD)/test_operands.o
	$(CC) $(CFLAGS) $^ $(CAPSTONE_LIBS) -o $@

$(BUILD)/test_dataflow_emu: $(BUILD)/dataflow.o $(BUILD)/dataflow_operands.o \
                            $(BUILD)/dataflow_emu.o $(BUILD)/test_dataflow_emu.o
	$(CC) $(CFLAGS) $^ $(UNICORN_LIBS) $(CAPSTONE_LIBS) -o $@

# --- the tier target -------------------------------------------------------
# Gate the emulator suite on libunicorn (like emu-test); the pure suites always run.
DF_HAVE_UNICORN := $(shell pkg-config --exists unicorn 2>/dev/null && echo 1)
ifeq ($(DF_HAVE_UNICORN),1)
DF_EMU_SUITE := $(BUILD)/test_dataflow_emu
endif

.PHONY: dataflow-test dataflow-grep-gate
dataflow-test: $(BUILD)/test_dataflow $(BUILD)/test_operands $(DF_EMU_SUITE)
	@echo "== dataflow-test =="
	$(MAKE) --no-print-directory dataflow-grep-gate
	$(BUILD)/test_dataflow
	$(BUILD)/test_operands
ifeq ($(DF_HAVE_UNICORN),1)
	$(BUILD)/test_dataflow_emu
else
	@echo "# SKIP test_dataflow_emu: no libunicorn (make deps DEPS_ARGS=--emu)"
endif

# Phase 0 exit-criterion grep gate: the operand enumerator must hold ONE persistent
# csh, never a per-op cs_open in the hot path — so exactly one cs_open call site.
dataflow-grep-gate:
	@n=$$(grep -c 'cs_open(' src/dataflow_operands.c); \
	 if [ "$$n" -ne 1 ]; then \
	   echo "dataflow: expected exactly one cs_open (persistent handle), found $$n"; \
	   exit 1; \
	 fi; \
	 echo "dataflow: operand enumerator holds a persistent csh (1 cs_open) — OK"
