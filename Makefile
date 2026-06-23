# asm-test — Phase 0 build (x86-64 macOS, GAS syntax via clang).
#
#   make test        build and run the example suite (green)
#   make demo-fail   build and run the intentional-failure demo
#   make clean       remove build artifacts

CC      := clang
AS      := clang            # clang assembles GAS-syntax .s on macOS
CFLAGS  := -Wall -Wextra -O0 -g -Iinclude
ASFLAGS :=
BUILD   := build

FRAMEWORK_OBJ := $(BUILD)/asmtest.o
ADD_OBJ       := $(BUILD)/add.o

.PHONY: all test demo-fail clean
all: test

$(BUILD):
	mkdir -p $(BUILD)

$(FRAMEWORK_OBJ): src/asmtest.c include/asmtest.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(ADD_OBJ): examples/add.s | $(BUILD)
	$(AS) $(ASFLAGS) -c $< -o $@

$(BUILD)/test_arith.o: examples/test_arith.c include/asmtest.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/test_arith: $(FRAMEWORK_OBJ) $(ADD_OBJ) $(BUILD)/test_arith.o
	$(CC) $(CFLAGS) $^ -o $@

test: $(BUILD)/test_arith
	./$(BUILD)/test_arith

$(BUILD)/test_failure_demo.o: examples/test_failure_demo.c include/asmtest.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/test_failure_demo: $(FRAMEWORK_OBJ) $(ADD_OBJ) $(BUILD)/test_failure_demo.o
	$(CC) $(CFLAGS) $^ -o $@

# Expected to exit nonzero; the leading '-' keeps make from erroring out.
demo-fail: $(BUILD)/test_failure_demo
	-./$(BUILD)/test_failure_demo

clean:
	rm -rf $(BUILD)
