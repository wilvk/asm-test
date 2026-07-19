# pintools/libdft_oracle/makefile — out-of-kit build stub for the libdft64 oracle tool.
#
# Derived from the pinned Pin kit's source/tools/MyPinTool/makefile (Intel,
# SPDX-License-Identifier: MIT — its scaffolding is safe to derive from; see
# licenses/README.md) and libdft64's tools/makefile. The kit's out-of-kit protocol:
# point PIN_ROOT at the kit root, include its makefile.config, this dir's makefile.rules
# (which adds the libdft include/lib paths), then the kit's makefile.default.rules — i.e.
#     make PIN_ROOT=/opt/pin/pin-external-<ver>-gcc-linux \
#          LIBDFT_ROOT=/opt/libdft64 obj-intel64/oracle.so
# The tool compiles under PinCRT (-DPIN_CRT=1 -nostdlib -fno-exceptions -fno-rtti,
# injected by the kit's makefile.unix.config) and links libdft.a (-ldft). Test/oracle-only.
ifndef PIN_ROOT
$(error PIN_ROOT must point at the pinned Pin kit root (Dockerfile.taint-oracle sets it))
endif

CONFIG_ROOT := $(PIN_ROOT)/source/tools/Config
include $(CONFIG_ROOT)/makefile.config
include makefile.rules
include $(TOOLS_ROOT)/Config/makefile.default.rules
