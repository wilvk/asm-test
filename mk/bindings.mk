# bindings.mk — Cross-language conformance corpus, per-language binding tests, and packaging.
#
# Included by ../Makefile (split out by concern for maintainability). All
# variables/knobs (CSTD, WERROR, ASM_SYNTAX, BUILD, ...) come from the parent
# Makefile, which reads this file in place; edit targets here, knobs there.

# --- Cross-language conformance corpus (Track 0.4) -------------------------
# The canonical-routine corpus + its C reference runner: the single source of
# truth every language binding must reproduce. Drives the routines through the
# binding-ABI entry points (asm_call_capture* + emu_call) and checks each result
# against the expected literal, then emits the portable table to corpus.json.
# Links the runtime built -DASMTEST_NO_MAIN (so its main() doesn't collide) plus
# the emulator; requires libunicorn, like `make emu-test`.
# Without main(), the runner-only static helpers (install_handlers, run_forked,
# the JUnit/TAP printers, the CLI parser, ...) are legitimately unused, so quiet
# that one warning for this build only.
$(BUILD)/asmtest_nomain.o: src/asmtest.c include/asmtest.h | $(BUILD)
	$(CC) $(CFLAGS) -DASMTEST_NO_MAIN -Wno-unused-function -c $< -o $@

$(BUILD)/conformance.o: bindings/conformance/conformance.c include/asmtest.h \
                        include/asmtest_emu.h | $(BUILD)
	$(CC) $(CFLAGS) $(UNICORN_CFLAGS) -c $< -o $@

$(BUILD)/conformance: $(BUILD)/conformance.o $(BUILD)/asmtest_nomain.o \
                      $(BUILD)/capture.o $(BUILD)/emu.o $(BUILD)/ffi.o \
                      $(BUILD)/trace.o $(BUILD)/add.o $(BUILD)/flags.o \
                      $(BUILD)/fp.o $(BUILD)/simd.o $(BUILD)/fault.o
	$(CC) $(CFLAGS) $^ $(UNICORN_LIBS) -o $@

conformance: $(BUILD)/conformance
	./$(BUILD)/conformance
	./$(BUILD)/conformance --emit > bindings/conformance/corpus.json
	@echo "conformance: wrote bindings/conformance/corpus.json"

# --- Binding function-surface parity (Track 0.5) ---------------------------
# The conformance corpus pins binding BEHAVIOUR and the manifest pins struct
# LAYOUT; this pins the FUNCTION SURFACE of the native-trace tiers — every
# binding must wrap every symbol in asmtest_hwtrace.h / asmtest_drtrace.h, so a
# new entry point can't be wired into nine bindings and missed in the tenth.
# Pure git-grep over the headers + binding sources: no toolchain, no build.
# Intentional omissions live in scripts/bindings-parity-allow.txt.
check-bindings-parity:
	@scripts/check-bindings-parity.sh

bindings-parity-report:
	@scripts/check-bindings-parity.sh --report

# In-line-assembler variant: the same reference runner built -DASMTEST_ENABLE_ASM
# and linked against the assembler (assemble.o, -lkeystone), so the optional asm
# tier (asm.add_signed / asm.att_3arg / asm.bad_source / asm.arm64_bytes) is
# compiled in and actually executes — the C-side anchor for the asm cases the
# bindings test. Does not re-emit corpus.json (the asm cases are emitted there
# unconditionally by the base `conformance` target).
$(BUILD)/conformance_asm.o: bindings/conformance/conformance.c include/asmtest.h \
                            include/asmtest_emu.h include/asmtest_assemble.h | $(BUILD)
	$(CC) $(CFLAGS) $(UNICORN_CFLAGS) $(KEYSTONE_CFLAGS) -DASMTEST_ENABLE_ASM \
	      -c $< -o $@

$(BUILD)/conformance_asm: $(BUILD)/conformance_asm.o $(BUILD)/asmtest_nomain.o \
                          $(BUILD)/capture.o $(BUILD)/emu.o $(BUILD)/ffi.o \
                          $(BUILD)/trace.o $(BUILD)/assemble.o $(BUILD)/add.o \
                          $(BUILD)/flags.o $(BUILD)/fp.o $(BUILD)/simd.o \
                          $(BUILD)/fault.o
	$(CC) $(CFLAGS) $^ $(UNICORN_LIBS) $(KEYSTONE_LIBS) -o $@

conformance-asm: $(BUILD)/conformance_asm
	./$(BUILD)/conformance_asm

# --- Python binding (Track P) ----------------------------------------------
# A pure-ctypes binding that loads the shared lib + manifest and replays the
# conformance corpus from Python. `make python-test` builds the shared libs, the
# manifest, the corpus, and a fixture lib exporting the canonical routines as
# symbols (the "code under test" a binding dlsym()s), then runs pytest. Requires
# python3 + pytest, and libunicorn (for the emulator cases), like `make emu-test`.
PYTEST ?= python3 -m pytest
ifeq ($(UNAME_S),Darwin)
CORPUS_LIB     := $(BUILD)/libasmtest_corpus.dylib
CORPUS_LDFLAGS := -dynamiclib
else
CORPUS_LIB     := $(BUILD)/libasmtest_corpus.so
CORPUS_LDFLAGS := -shared
endif
CORPUS_ROUTINE_OBJS := $(BUILD)/pic/add.o $(BUILD)/pic/flags.o \
                       $(BUILD)/pic/fp.o $(BUILD)/pic/simd.o \
                       $(BUILD)/pic/fault.o $(BUILD)/pic/corpus_routines.o

# name -> routine-address lookup, so bindings need no per-FFI symbol-address API.
$(BUILD)/pic/corpus_routines.o: bindings/conformance/corpus_routines.c | $(BUILD)/pic
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(CORPUS_LIB): $(CORPUS_ROUTINE_OBJS)
	$(CC) $(CFLAGS) $(CORPUS_LDFLAGS) $^ -o $@

python-test: shared-emu manifest conformance $(CORPUS_LIB)
	cd bindings/python && \
	  ASMTEST_LIB=$(abspath $(call shlib_dev,libasmtest_emu)) \
	  ASMTEST_MANIFEST=$(abspath asmtest_abi.json) \
	  ASMTEST_CORPUS_JSON=$(abspath bindings/conformance/corpus.json) \
	  ASMTEST_CORPUS_LIB=$(abspath $(CORPUS_LIB)) \
	  $(PYTEST) -q

# --- C++ binding (Track X) -------------------------------------------------
# The C headers are C++-consumable (extern "C" guards); bindings/cpp/asmtest.hpp
# adds RAII + typed conveniences. The example suite drives the framework from a
# C++ TU and links the same framework objects as the C suites. `make cpp-test`
# builds and runs it; requires a C++ compiler and libunicorn (emulator case).
CXX      ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O0 -g -Iinclude

# Full build: the bindings base now carries Keystone + Capstone (libasmtest_emu is
# the superset), so the C++ example enables emulator + in-line assembler + disas
# by default — matching the other bindings, where asm/disas are on out of the box.
$(BUILD)/test_cpp.o: bindings/cpp/test_cpp.cpp bindings/cpp/asmtest.hpp \
                     include/asmtest.h include/asmtest_emu.h \
                     include/asmtest_assemble.h | $(BUILD)
	$(CXX) $(CXXFLAGS) $(UNICORN_CFLAGS) $(KEYSTONE_CFLAGS) $(CAPSTONE_CFLAGS) \
	       $(CAPSTONE_DEF) -DASMTEST_ENABLE_EMU -DASMTEST_ENABLE_ASM \
	       -DASMTEST_ENABLE_DISAS -c $< -o $@

$(BUILD)/test_cpp: $(FRAMEWORK_OBJS) $(BUILD)/emu.o $(BUILD)/trace.o \
                   $(BUILD)/fuzz.o $(BUILD)/assemble.o $(BUILD)/disasm.o \
                   $(BUILD)/add.o $(BUILD)/flags.o $(BUILD)/fp.o \
                   $(BUILD)/simd.o $(BUILD)/fault.o $(BUILD)/test_cpp.o
	$(CXX) $(CXXFLAGS) $^ $(UNICORN_LIBS) $(KEYSTONE_LIBS) $(CAPSTONE_LIBS) -o $@

cpp-test: $(BUILD)/test_cpp
	./$(BUILD)/test_cpp

# --- Rust binding (Track R) ------------------------------------------------
# A no-dependency crate (#[repr(C)] structs + extern "C" over the binding ABI)
# linked against the shared libs. `make rust-test` builds the shared libs + the
# routine fixture lib, then runs `cargo test`; requires cargo + libunicorn.
CARGO ?= cargo
rust-test: shared-emu $(CORPUS_LIB)
	cd bindings/rust && \
	  ASMTEST_LIB_DIR=$(abspath $(BUILD)) \
	  LD_LIBRARY_PATH="$(abspath $(BUILD)):$$LD_LIBRARY_PATH" \
	  DYLD_LIBRARY_PATH="$(abspath $(BUILD)):$$DYLD_LIBRARY_PATH" \
	  $(CARGO) test

# --- Zig binding (Track Z) -------------------------------------------------
# Zig consumes the C headers directly via @cImport — no separate binding layer.
# `make zig-test` builds the shared libs + the routine fixture lib, then runs
# `zig build test`; requires zig + libunicorn. (build.zig targets Zig 0.13.x.)
ZIG ?= zig
zig-test: shared-emu $(CORPUS_LIB)
	cd bindings/zig && \
	  LD_LIBRARY_PATH="$(abspath $(BUILD)):$$LD_LIBRARY_PATH" \
	  DYLD_LIBRARY_PATH="$(abspath $(BUILD)):$$DYLD_LIBRARY_PATH" \
	  $(ZIG) build test -Dincdir=$(abspath include) -Dlibdir=$(abspath $(BUILD))

# --- Community / managed-runtime bindings (Tracks N, J, D, C, G) ------------
# Each replays the conformance corpus through the opaque-handle FFI layer (no
# struct layout): asmtest_corpus_routine for addresses, asmtest_capture6/_fp2 +
# accessors for capture, asmtest_emu_call2 + accessors for the emulator. They
# need only the shared emulator lib + the routine fixture lib; their toolchains
# live in the Docker bindings image (use `make docker-ruby` / `-lua` / `-node` /
# `-java` / `-dotnet` / `-go`). Shared env points the loader at the build dir.
RUBY   ?= ruby
LUAJIT ?= luajit
NODE   ?= node
JAVAC  ?= javac
JAVA   ?= java
DOTNET ?= dotnet
GO     ?= go
bindings_env = ASMTEST_LIB=$(abspath $(call shlib_dev,libasmtest_emu)) \
               ASMTEST_CORPUS_LIB=$(abspath $(CORPUS_LIB)) \
               LD_LIBRARY_PATH="$(abspath $(BUILD)):$$LD_LIBRARY_PATH" \
               DYLD_LIBRARY_PATH="$(abspath $(BUILD)):$$DYLD_LIBRARY_PATH"

# libasmtest_emu is the superset, so bindings_env resolves the assembler +
# disassembler too — asm_available()/disas_available() are true and every
# binding's `<lang>-test` exercises asm and disas (no separate asm env/target).
ruby-test: shared-emu $(CORPUS_LIB)
	$(bindings_env) $(RUBY) bindings/ruby/conformance.rb

lua-test: shared-emu $(CORPUS_LIB)
	$(bindings_env) $(LUAJIT) bindings/lua/conformance.lua

node-test: shared-emu $(CORPUS_LIB)
	$(bindings_env) $(NODE) bindings/node/conformance.js

java-test: shared-emu $(CORPUS_LIB)
	mkdir -p $(BUILD)/java
	$(JAVAC) --release 21 --enable-preview -d $(BUILD)/java \
	  bindings/java/Asmtest.java bindings/java/Conformance.java
	$(bindings_env) $(JAVA) --enable-preview --enable-native-access=ALL-UNNAMED \
	  -cp $(BUILD)/java Conformance

dotnet-test: shared-emu $(CORPUS_LIB)
	$(bindings_env) $(DOTNET) run --project bindings/dotnet/asmtest.csproj

# Go links the shared libs at build time via cgo; CGO_LDFLAGS carries the -L (so
# a custom BUILD works), and bindings_env's LD_LIBRARY_PATH/DYLD_LIBRARY_PATH
# resolves them at run time. GOTOOLCHAIN=local + GOPROXY=off keep it offline (no
# module deps). The emulator case uses the x86-64 guest, so run on an x86-64 host.
go-test: shared-emu $(CORPUS_LIB)
	cd bindings/go && CGO_LDFLAGS="-L$(abspath $(BUILD))" \
	  GOTOOLCHAIN=local GOFLAGS=-mod=mod GOPROXY=off \
	  $(bindings_env) $(GO) test ./...

# --- Packaging scaffolding (publishable artifacts) -------------------------
# See docs/packaging.md. `make package-libs` stages the host's shared libs into
# build/dist/native/<plat>/; each `make <lang>-package` re-stages into that
# ecosystem's native-payload location and runs its packer, emitting under
# build/dist/<lang>/. Scaffolding only: a multi-platform release repeats the
# native staging per target OS/arch (or uses the ecosystem's prebuild tooling),
# and `make packages` needs every toolchain (prefer one language at a time, or
# each binding's Docker image).
PKG_DIST   := $(BUILD)/dist
PKG_PLAT   := $(shell uname -s | tr '[:upper:]' '[:lower:]')-$(shell uname -m)
DOTNET_RID ?= $(PKG_PLAT)
GEM      ?= gem
NPM      ?= npm
CARGO    ?= cargo
JAR      ?= jar
LUAROCKS ?= luarocks
PYBUILD  ?= python3 -m build
# The unversioned names the dlopen bindings look up (libasmtest_emu.dylib, ...).
pkg_emu_name  := $(notdir $(call shlib_dev,libasmtest_emu))
pkg_core_name := $(notdir $(call shlib_dev,libasmtest))

.PHONY: packages package-libs package-libs-verify native-payload-check \
        python-package rust-package zig-package cpp-package node-package \
        java-package dotnet-package ruby-package lua-package go-package

# The dlopen packers consume build/dist/native/ (staged by `make package-libs`
# locally, or the CI payloads matrix downloaded into it) rather than depending on
# package-libs directly — so a release job can package the collected
# multi-platform tree without rebuilding the host slot (which would need the full
# Unicorn + Keystone + Capstone toolchain). This guard fails fast if the tree is
# absent.
native-payload-check:
	@ls -d $(PKG_DIST)/native/*/ >/dev/null 2>&1 || { \
	  echo "native-payload-check: no payload in $(PKG_DIST)/native — run 'make package-libs' first"; exit 1; }

# package-libs stages the build host's native payload into build/dist/native/<plat>/.
# libasmtest_emu IS the full superset (emu + Keystone assembler + Capstone
# disassembler), so it is staged directly under that slot and scripts/package-native.sh
# then vendors its three native deps beside it (rpath -> $$ORIGIN/@loader_path) and
# assembles THIRD-PARTY-LICENSES, so a fresh install runs both optional tiers with no
# system libs. Needs libunicorn + libkeystone + libcapstone at build time (make deps
# DEPS_ARGS=--asm plus the build-{keystone,capstone}.sh source builds).
package-libs: shared shared-emu
	mkdir -p $(PKG_DIST)/native/$(PKG_PLAT)
	cp -f $(call shlib_real,libasmtest)     $(PKG_DIST)/native/$(PKG_PLAT)/$(pkg_core_name)
	cp -f $(call shlib_real,libasmtest_emu) $(PKG_DIST)/native/$(PKG_PLAT)/$(pkg_emu_name)
	sh scripts/package-native.sh $(PKG_DIST)/native/$(PKG_PLAT) $(pkg_emu_name)
	@echo "package-libs: staged $(PKG_PLAT) full payload in $(PKG_DIST)/native/$(PKG_PLAT)"

# dlopen bindings: bundle the prebuilt libasmtest_emu (the superset) + its vendored
# native deps + THIRD-PARTY-LICENSES in the package's payload. Python's wheel is
# per-platform (cibuildwheel builds one per tag in CI), so it bundles only the host
# slot, flat in asmtest/_libs/ (where _native.py looks); auditwheel/delocate vendor
# the deps at repair time, and the notices ride along.
python-package: package-libs manifest
	rm -rf bindings/python/asmtest/_libs bindings/python/build bindings/python/*.egg-info
	mkdir -p bindings/python/asmtest/_libs $(PKG_DIST)/python
	# Stage the RAW (system-linked) superset + core libs, NOT the pre-vendored
	# build/dist copies: the wheel's deps are vendored by auditwheel (Linux) /
	# delocate (macOS) at repair time, and those tools resolve the lib's
	# dependencies from the system — they choke on a lib already rewritten to
	# @loader_path. The THIRD-PARTY-LICENSES notices still ride along.
	cp -f $(call shlib_real,libasmtest_emu) bindings/python/asmtest/_libs/$(pkg_emu_name)
	cp -f $(call shlib_real,libasmtest)     bindings/python/asmtest/_libs/$(pkg_core_name)
	cp -f asmtest_abi.json bindings/python/asmtest/_libs/
	cp -Rf $(PKG_DIST)/native/$(PKG_PLAT)/THIRD-PARTY-LICENSES bindings/python/asmtest/_libs/ 2>/dev/null || true
	cd bindings/python && $(PYBUILD) --wheel --outdir $(abspath $(PKG_DIST))/python

# Each dlopen packer ships the reusable library module (asmtest.rb / asmtest.js /
# asmtest.lua / Asmtest.java / AsmTest.dll), NOT the conformance runner, and
# bundles one native slot per platform present in build/dist/native/ (staged by
# package-libs locally; the CI payloads matrix collects all four). emu_lib_slots
# copies the whole runtime payload from each <plat> slot — libasmtest_emu, the
# vendored Unicorn/Keystone/Capstone, and THIRD-PARTY-LICENSES — into a per-platform
# dir (the rpath patching staged at package-libs time keeps the deps resolvable).
define emu_lib_slots
	@for pd in $(PKG_DIST)/native/*/; do \
	  [ -d "$$pd" ] || continue; p=$$(basename "$$pd"); \
	  mkdir -p "$(1)/$$p"; \
	  cp -f "$$pd"lib*.so* "$(1)/$$p/" 2>/dev/null || true; \
	  cp -f "$$pd"lib*.dylib "$(1)/$$p/" 2>/dev/null || true; \
	  cp -Rf "$$pd"THIRD-PARTY-LICENSES "$(1)/$$p/" 2>/dev/null || true; \
	  echo "  bundled $$p (lib + vendored deps + licenses)"; \
	done
endef

ruby-package: native-payload-check
	rm -rf bindings/ruby/native && mkdir -p $(PKG_DIST)/ruby
	$(call emu_lib_slots,bindings/ruby/native)
	cd bindings/ruby && $(GEM) build asmtest.gemspec
	mv bindings/ruby/asmtest-$(ASMTEST_VERSION).gem $(PKG_DIST)/ruby/

node-package: native-payload-check
	rm -rf bindings/node/native && mkdir -p $(PKG_DIST)/node
	$(call emu_lib_slots,bindings/node/native)
	cd bindings/node && $(NPM) pack --pack-destination $(abspath $(PKG_DIST))/node

java-package: native-payload-check
	rm -rf bindings/java/src/main/resources/native
	mkdir -p $(BUILD)/java-pkg $(PKG_DIST)/java
	$(call emu_lib_slots,bindings/java/src/main/resources/native)
	$(JAVAC) --release 21 --enable-preview -d $(BUILD)/java-pkg bindings/java/Asmtest.java
	cp -r bindings/java/src/main/resources/native $(BUILD)/java-pkg/
	cd $(BUILD)/java-pkg && $(JAR) cf $(abspath $(PKG_DIST))/java/asmtest-$(ASMTEST_VERSION).jar .

# .NET: stage a runtimes/<rid>/native/ slot per platform (mapping the <os>-<arch>
# payload name to the .NET RID the loader resolves), then `dotnet pack` the
# classlib project (asmtest-lib.csproj) — it compiles the AsmTest.dll library from
# Asmtest.cs and packs it + those native assets into a real nupkg the consumer
# restores (the loader picks runtimes/<rid>/native/ at run time).
dotnet-package: native-payload-check
	rm -rf bindings/dotnet/runtimes
	mkdir -p $(PKG_DIST)/dotnet
	@for pd in $(PKG_DIST)/native/*/; do \
	  [ -d "$$pd" ] || continue; p=$$(basename "$$pd"); \
	  rid=$$(echo "$$p" | sed -e 's/^darwin-/osx-/' -e 's/-x86_64$$/-x64/' -e 's/-aarch64$$/-arm64/'); \
	  mkdir -p bindings/dotnet/runtimes/$$rid/native; \
	  cp -f "$$pd"lib*.so* bindings/dotnet/runtimes/$$rid/native/ 2>/dev/null || true; \
	  cp -f "$$pd"lib*.dylib bindings/dotnet/runtimes/$$rid/native/ 2>/dev/null || true; \
	  cp -Rf "$$pd"THIRD-PARTY-LICENSES bindings/dotnet/runtimes/$$rid/native/ 2>/dev/null || true; \
	  echo "  bundled $$p -> runtimes/$$rid/native (lib + vendored deps + licenses)"; \
	done
	$(DOTNET) pack bindings/dotnet/asmtest-lib.csproj -c Release \
	  -p:PackageOutputPath=$(abspath $(PKG_DIST))/dotnet
	@echo "dotnet-package: packed AsmTest nupkg (lib/net8.0/AsmTest.dll + runtimes/<rid>/native) in $(PKG_DIST)/dotnet"

lua-package: native-payload-check
	rm -rf bindings/lua/native && mkdir -p $(PKG_DIST)/lua
	$(call emu_lib_slots,bindings/lua/native)
	cp -f bindings/lua/asmtest-1.0.0-1.rockspec $(PKG_DIST)/lua/
	@echo "lua-package: staged rockspec + native in $(PKG_DIST)/lua (luarocks pack/upload to publish)"

# link bindings: source distributions (the consumer builds/installs libasmtest).
rust-package:
	mkdir -p $(PKG_DIST)/rust
	cd bindings/rust && CARGO_TARGET_DIR=$(abspath $(BUILD))/rust-pkg \
	  $(CARGO) package --no-verify --allow-dirty
	cp $(BUILD)/rust-pkg/package/*.crate $(PKG_DIST)/rust/

zig-package:
	mkdir -p $(PKG_DIST)/zig
	tar czf $(PKG_DIST)/zig/asmtest-zig-$(ASMTEST_VERSION).tar.gz \
	  -C bindings/zig build.zig build.zig.zon src README.md

cpp-package:
	mkdir -p $(PKG_DIST)/cpp
	tar czf $(PKG_DIST)/cpp/asmtest-cpp-$(ASMTEST_VERSION).tar.gz \
	  -C bindings/cpp asmtest.hpp CMakeLists.txt README.md

go-package:
	@echo "go-package: Go modules publish from the tagged repo (no artifact to build)."
	@echo "  module: github.com/wilvk/asm-test/bindings/go"
	@echo "  consumers set CGO_LDFLAGS to link libasmtest_emu (see docs/packaging.md)."

packages: package-libs python-package rust-package zig-package cpp-package node-package \
          java-package dotnet-package ruby-package lua-package go-package

# One-shot LOCAL packaging: `make <lang>-package-full` runs every step needed to
# emit a complete package on this host — it builds + stages the host's native
# payload (package-libs: the superset emu lib + vendored Unicorn/Keystone/Capstone
# + THIRD-PARTY-LICENSES) first, then runs the packer. The plain `<lang>-package`
# targets stay payload-consumers (gated by native-payload-check) so a CI release
# host can package a downloaded multi-platform `native-all` tree without the full
# toolchain; the `-full` aliases are the convenience entrypoint for a single-box
# local build. Needs libunicorn + libkeystone + libcapstone (make deps).
#
# dlopen packagers need the staged payload, so their -full alias prepends
# package-libs. python-package already depends on package-libs, so its alias just
# forwards. The link/source packagers (rust/zig/cpp/go) bundle no native payload,
# so their -full alias is the plain target — provided for a uniform interface.
.PHONY: python-package-full ruby-package-full node-package-full java-package-full \
        dotnet-package-full lua-package-full rust-package-full zig-package-full \
        cpp-package-full go-package-full

ruby-package-full:   package-libs ; $(MAKE) ruby-package
node-package-full:   package-libs ; $(MAKE) node-package
java-package-full:   package-libs ; $(MAKE) java-package
dotnet-package-full: package-libs ; $(MAKE) dotnet-package
lua-package-full:    package-libs ; $(MAKE) lua-package

python-package-full: python-package
rust-package-full:   rust-package
zig-package-full:    zig-package
cpp-package-full:    cpp-package
go-package-full:     go-package

# Verify a (possibly multi-platform) build/dist/native/ tree carries a complete
# fully-featured set: every <plat> subdir must hold the core lib, the libasmtest_emu
# superset the dlopen bindings load, the three vendored native deps
# (Unicorn/Keystone/Capstone) that make it self-contained, and a THIRD-PARTY-LICENSES
# dir. `make package-libs` stages only the build host's slot; the CI `payloads`
# matrix runs it on each OS/arch and the collect job merges the artifacts into one
# tree, then runs this target so a release never ships a payload missing a platform,
# a lib, a vendored dep, or its notices. File-existence only (it runs on the collect
# host, which is one platform — the per-slot symbol assertion happens at
# package-libs time via scripts/package-native.sh). Exits nonzero if any slot is
# incomplete.
package-libs-verify:
	@dir=$(PKG_DIST)/native; \
	test -d "$$dir" || { echo "package-libs-verify: no $$dir — run 'make package-libs' first"; exit 1; }; \
	plats=$$(cd "$$dir" && ls -d */ 2>/dev/null | tr -d /); \
	test -n "$$plats" || { echo "package-libs-verify: $$dir has no platform subdirs"; exit 1; }; \
	rc=0; n=0; \
	for p in $$plats; do \
	  n=$$((n+1)); pd="$$dir/$$p"; miss=""; \
	  core=$$(ls "$$pd"/libasmtest.so* "$$pd"/libasmtest.*.dylib "$$pd"/libasmtest.dylib 2>/dev/null | head -1); \
	  emu=$$(ls "$$pd"/libasmtest_emu.so* "$$pd"/libasmtest_emu.dylib 2>/dev/null | grep -v _emu_ | head -1); \
	  [ -n "$$core" ] || miss="$$miss core"; \
	  [ -n "$$emu" ] || miss="$$miss emu"; \
	  for d in unicorn keystone capstone; do \
	    ls "$$pd"/lib$$d.* >/dev/null 2>&1 || miss="$$miss $$d"; \
	  done; \
	  [ -d "$$pd/THIRD-PARTY-LICENSES" ] || miss="$$miss licenses"; \
	  if [ -z "$$miss" ]; then \
	    echo "  ok   $$p   (core, emu, unicorn, keystone, capstone, licenses)"; \
	  else \
	    echo "  MISS $$p  missing:$$miss"; rc=1; \
	  fi; \
	done; \
	echo "package-libs-verify: $$n platform(s) in $$dir"; \
	exit $$rc

