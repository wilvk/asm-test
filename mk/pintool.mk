# pintool.mk — XED-decoded Intel Pin trace tier (PIN-2, pin-xed-trace-tier.md).
#
# Included by ../Makefile after mk/native-trace.mk (so HWTRACE_OBJS / DRAPP_KS_OBJ
# / DR_AVAILABLE are already defined for the T6 parity validator link). Knobs live
# in the parent Makefile / mk/docker.mk; edit targets here.
#
# The tier builds pintool/asmtest_pintool.so against a pinned, digest-gated Pin kit
# via Pin's out-of-kit PIN_ROOT make protocol (pintool/makefile), launches a fixture
# under `pin -t`, and asserts the recorded instruction/block offsets are
# byte-identical to the in-process single-step and DynamoRIO backends. Pin decodes
# with XED, so it traces Intel APX where the DynamoRIO decoder aborts (T8).
#
# The tool compiles under PinCRT (-DPIN_CRT=1 -nostdlib -fno-exceptions -fno-rtti,
# injected by the kit's makefile.unix.config), so pintool/asmtest_pintool.cpp sticks
# to the PinCRT subset — plain POSIX open/mmap, no libstdc++ iostream.
#
# REAL gates (CLAUDE.md): the pinned Pin kit is x86-64 gcc-linux, so every runnable
# target self-skips (printed reason + `1..0 # skipped`) on a non-x86-64 or non-Linux
# host — nothing to apt-install, an architecture gate like mk/cli.mk's CLI_ARCH. Pin
# itself IS installable, so on a gate-passing host a missing kit is fetched via
# scripts/fetch-pin.sh rather than skipped.

PINTOOL_ARCH := $(shell uname -m)
PINTOOL_SO   := pintool/obj-intel64/asmtest_pintool.so

.PHONY: pintool-tool pintool-test

ifneq ($(PINTOOL_ARCH),x86_64)

pintool-tool pintool-test:
	@echo "# SKIP pintool: Intel Pin is x86-only and this host is $(PINTOOL_ARCH)."
	@echo "1..0 # skipped"

else ifneq ($(UNAME_S),Linux)

pintool-tool pintool-test:
	@echo "# SKIP pintool: the pinned Pin kit is gcc-linux; this host is $(UNAME_S)."
	@echo "1..0 # skipped"

else

# Build the out-of-kit tool. A missing Pin kit is an installable dependency, so the
# recipe fetches it (CLAUDE.md rule); PIN_HOME overrides, and the docker lane always
# sets it. Built via the kit's out-of-kit protocol (pintool/makefile).
pintool-tool:
	@home=$${PIN_HOME:-$$(sh scripts/fetch-pin.sh)}; \
	echo "== pintool-tool (PIN_ROOT=$$home) =="; \
	cd pintool && $(MAKE) PIN_ROOT=$$home obj-intel64/asmtest_pintool.so
	@echo "built $(PINTOOL_SO)"

endif
