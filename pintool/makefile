# pintool/makefile — out-of-kit build stub for asmtest_pintool.so.
#
# Derived from the pinned Pin kit's source/tools/MyPinTool/makefile (Intel,
# SPDX-License-Identifier: MIT — its scaffolding is safe to derive from; see
# licenses/README.md). The kit's out-of-kit protocol: point PIN_ROOT at the kit
# root, include its makefile.config, this dir's makefile.rules, then the kit's
# makefile.default.rules — i.e.
#     make PIN_ROOT=/opt/pin/pin-external-<ver>-gcc-linux obj-intel64/asmtest_pintool.so
# mk/pintool.mk's `pintool-tool` target invokes exactly that. The tool compiles
# under PinCRT (-DPIN_CRT=1 -nostdlib -fno-exceptions -fno-rtti, injected by the
# kit's makefile.unix.config), so asmtest_pintool.cpp sticks to the PinCRT subset.
ifndef PIN_ROOT
$(error PIN_ROOT must point at the pinned Pin kit root (mk/pintool.mk sets it))
endif

CONFIG_ROOT := $(PIN_ROOT)/source/tools/Config
include $(CONFIG_ROOT)/makefile.config
include makefile.rules
include $(TOOLS_ROOT)/Config/makefile.default.rules
