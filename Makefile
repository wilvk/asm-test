# asm-test — Phase 3 build (x86-64, Linux + macOS).
#
#   make test        build and run the example suites (green)
#   make demo-fail   build and run the intentional-failure demo
#   make clean       remove build artifacts
#
# CC defaults to the system compiler (clang on macOS, gcc on Linux); both
# assemble the GAS-syntax .s sources. The .s files are run through the C
# preprocessor (-x assembler-with-cpp) so they can #include "asm.h".

CC      ?= cc
CFLAGS  := -Wall -Wextra -O0 -g -Iinclude
ASFLAGS := -x assembler-with-cpp -Iinclude
BUILD   := build

# Framework runtime: C runner + the asm capture trampoline.
FRAMEWORK_OBJS := $(BUILD)/asmtest.o $(BUILD)/capture.o
SUITES         := $(BUILD)/test_arith $(BUILD)/test_mem $(BUILD)/test_capture

.PHONY: all test demo-fail clean
all: test

$(BUILD):
	mkdir -p $(BUILD)

# Framework objects.
$(BUILD)/asmtest.o: src/asmtest.c include/asmtest.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/capture.o: src/capture.s include/asm.h | $(BUILD)
	$(CC) $(CFLAGS) $(ASFLAGS) -c $< -o $@

# Generic rules: assemble example .s (with cpp) and compile example .c.
$(BUILD)/%.o: examples/%.s include/asm.h | $(BUILD)
	$(CC) $(CFLAGS) $(ASFLAGS) -c $< -o $@

$(BUILD)/%.o: examples/%.c include/asmtest.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# One test binary per suite: framework + routine(s) + test cases.
$(BUILD)/test_arith: $(FRAMEWORK_OBJS) $(BUILD)/add.o $(BUILD)/test_arith.o
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_mem: $(FRAMEWORK_OBJS) $(BUILD)/mem.o $(BUILD)/test_mem.o
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_capture: $(FRAMEWORK_OBJS) $(BUILD)/flags.o $(BUILD)/test_capture.o
	$(CC) $(CFLAGS) $^ -o $@

test: $(SUITES)
	@set -e; for t in $(SUITES); do echo "== $$t =="; ./$$t; done

# Expected to exit nonzero; the leading '-' keeps make from erroring out.
$(BUILD)/test_failure_demo: $(FRAMEWORK_OBJS) $(BUILD)/flags.o $(BUILD)/test_failure_demo.o
	$(CC) $(CFLAGS) $^ -o $@

demo-fail: $(BUILD)/test_failure_demo
	-./$(BUILD)/test_failure_demo

clean:
	rm -rf $(BUILD)
