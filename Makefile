# asm-test — Phase 1 build (x86-64 macOS, GAS syntax via clang).
#
#   make test        build and run the example suites (green)
#   make demo-fail   build and run the intentional-failure demo
#   make clean       remove build artifacts

CC      := clang
AS      := clang            # clang assembles GAS-syntax .s on macOS
CFLAGS  := -Wall -Wextra -O0 -g -Iinclude
ASFLAGS :=
BUILD   := build

FRAMEWORK_OBJ := $(BUILD)/asmtest.o
SUITES        := $(BUILD)/test_arith $(BUILD)/test_mem

.PHONY: all test demo-fail clean
all: test

$(BUILD):
	mkdir -p $(BUILD)

# Framework runtime.
$(FRAMEWORK_OBJ): src/asmtest.c include/asmtest.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# Generic rules: assemble .s and compile example .c into build/.
$(BUILD)/%.o: examples/%.s | $(BUILD)
	$(AS) $(ASFLAGS) -c $< -o $@

$(BUILD)/%.o: examples/%.c include/asmtest.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# One test binary per suite: framework + routine(s) + test cases.
$(BUILD)/test_arith: $(FRAMEWORK_OBJ) $(BUILD)/add.o $(BUILD)/test_arith.o
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_mem: $(FRAMEWORK_OBJ) $(BUILD)/mem.o $(BUILD)/test_mem.o
	$(CC) $(CFLAGS) $^ -o $@

test: $(SUITES)
	@set -e; for t in $(SUITES); do echo "== $$t =="; ./$$t; done

# Expected to exit nonzero; the leading '-' keeps make from erroring out.
$(BUILD)/test_failure_demo: $(FRAMEWORK_OBJ) $(BUILD)/add.o $(BUILD)/test_failure_demo.o
	$(CC) $(CFLAGS) $^ -o $@

demo-fail: $(BUILD)/test_failure_demo
	-./$(BUILD)/test_failure_demo

clean:
	rm -rf $(BUILD)
