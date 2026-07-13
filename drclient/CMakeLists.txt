# CMake sub-build for the DynamoRIO native-trace client (libasmtest_drclient.so).
#
# DynamoRIO ships no pkg-config file and requires find_package(DynamoRIO), so the
# client is the lone CMake-driven artifact the top-level Makefile shells out to
# (every other optional dep uses pkg-config). Point it at the DR cmake config dir:
#
#   cmake -DDynamoRIO_DIR=/path/to/DynamoRIO/cmake <srcdir>
#   cmake --build .
#
# C and CXX are both enabled because DynamoRIO's bundled DrMemoryFramework config
# reads CMAKE_CXX_SIZEOF_DATA_PTR; a C-only project leaves it empty and aborts.
cmake_minimum_required(VERSION 3.10)
project(asmtest_drclient C CXX)

find_package(DynamoRIO REQUIRED)

add_library(asmtest_drclient SHARED ${CMAKE_CURRENT_SOURCE_DIR}/../src/drtrace_client.c)
configure_DynamoRIO_client(asmtest_drclient)
# Raw core API only — no use_DynamoRIO_extension(): the prebuilt drmgr/drwrap/drreg
# fail to load under DR's private loader on modern glibc, and avoiding drwrap keeps
# the tier free of its LGPL-2.1 obligation.

# The data-flow L0 VALUE client (Phase 5): a SEPARATE in-process client that records
# per-instruction register/memory value snapshots (dr_get_mcontext + decode +
# opnd_compute_address + dr_safe_read) rather than block coverage. Same raw core-API
# constraint as the control client. It includes the shared capture ABI header from
# ../src (dataflow_dr.h — <stdint.h>-only, safe alongside dr_api.h), so add that dir.
add_library(asmtest_drval_client SHARED ${CMAKE_CURRENT_SOURCE_DIR}/../src/dataflow_dr_client.c)
configure_DynamoRIO_client(asmtest_drval_client)
target_include_directories(asmtest_drval_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

# The INLINED data-flow value client (Phase 5 / taint-tier Increment 3): the same
# at_drval_t capture as asmtest_drval_client, but re-platformed off the per-instruction
# clean call onto the BSD-clean extension stack (drmgr phased events + drreg scratch
# regs + drx_buf buffered trace) proved to load by Increment 2. NO umbra (LGPL-2.1),
# NO drwrap — marker/arg resolution stays PC-resolved (see the client header). The
# clean-call asmtest_drval_client stays intact as the oracle/fallback; point
# ASMTEST_DRVAL_CLIENT at either .so to A/B them against the emulator oracle.
add_library(asmtest_drval_client_inlined SHARED
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/dataflow_dr_client_inlined.c)
configure_DynamoRIO_client(asmtest_drval_client_inlined)
use_DynamoRIO_extension(asmtest_drval_client_inlined drmgr)
use_DynamoRIO_extension(asmtest_drval_client_inlined drreg)
use_DynamoRIO_extension(asmtest_drval_client_inlined drx)
target_include_directories(asmtest_drval_client_inlined PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

# The TAINT client (taint-tier Increment 4): the SAME TU built -DASMTEST_TAINT, layering
# the BSD 2-level tag shadow + per-thread reg-tag file + inline dst_tag = union(src_tags)
# propagation + on_seed on top of the value capture (which still runs byte-for-byte). Same
# BSD-clean drmgr/drreg/drx stack, NO umbra (LGPL-2.1), NO drwrap. It additionally needs
# the public taint ABI header (include/asmtest_taint.h), so add ../include. With the flag
# off this source compiles to asmtest_drval_client_inlined above; point ASMTEST_DRVAL_CLIENT
# at this .so to run the taint oracle diff (examples/dr_taint.c).
add_library(asmtest_drtaint_client SHARED
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/dataflow_dr_client_inlined.c)
configure_DynamoRIO_client(asmtest_drtaint_client)
use_DynamoRIO_extension(asmtest_drtaint_client drmgr)
use_DynamoRIO_extension(asmtest_drtaint_client drreg)
use_DynamoRIO_extension(asmtest_drtaint_client drx)
target_compile_definitions(asmtest_drtaint_client PRIVATE ASMTEST_TAINT)
target_include_directories(asmtest_drtaint_client PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../src ${CMAKE_CURRENT_SOURCE_DIR}/../include)

# The GC-REMAP taint client (taint-tier Increment 7, PARTIAL slice): the SAME TU as the
# taint client built additionally -DASMTEST_TAINT_GCREMAP, which compiles in the GC-move
# umbra shadow remap code path (at_gc_remap) and its synthetic-triple unit test (run via
# the `gcremap_selftest` client option). The flag is OFF for every other client, so this
# is a SEPARATE .so and the default value/taint clients stay byte-identical; the full live
# .NET GC path (Phase-4 {old,new,len} feed, coherence canary) is deferred / hard-blocked.
add_library(asmtest_drtaint_gcremap_client SHARED
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/dataflow_dr_client_inlined.c)
configure_DynamoRIO_client(asmtest_drtaint_gcremap_client)
use_DynamoRIO_extension(asmtest_drtaint_gcremap_client drmgr)
use_DynamoRIO_extension(asmtest_drtaint_gcremap_client drreg)
use_DynamoRIO_extension(asmtest_drtaint_gcremap_client drx)
target_compile_definitions(asmtest_drtaint_gcremap_client PRIVATE
  ASMTEST_TAINT ASMTEST_TAINT_GCREMAP)
target_include_directories(asmtest_drtaint_gcremap_client PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../src ${CMAKE_CURRENT_SOURCE_DIR}/../include)

# Emit straight into the asm-test build dir so the Makefile/tests find them.
set_target_properties(asmtest_drclient asmtest_drval_client asmtest_drval_client_inlined
  asmtest_drtaint_client asmtest_drtaint_gcremap_client
  PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${ASMTEST_BUILD_DIR}")

# --- Throwaway extension-load probe (taint tier, Increment 2) ---------------
# Opt-in only (-DASMTEST_BUILD_DREXT_PROBE=ON): the sole client here that DOES
# use_DynamoRIO_extension(), to empirically test whether the prebuilt release
# extensions load under DR's private loader — the documented-but-never-tested
# blocker gating the whole Phase-5 taint re-platform. It is NOT a product artifact
# and is never built by the normal `make drtrace-client` path (which stays raw
# core API). See drclient/probe_extensions.c and
# docs/internal/analysis/dr-extension-load-probe-findings.md.
#
# Default stack is the BSD-clean set only: drmgr + drreg + drx (the drx_buf
# trace-buffer API lives in drx.h — there is no separate drx_buf extension).
# The two LGPL-2.1 extensions each sit behind their own opt-in and stay out of the
# default probe and the CI gate:
#   -DPROBE_DRWRAP=ON  links drwrap (a DynamoRIO CORE ext/ extension — no separate
#                      find_package; use_DynamoRIO_extension resolves it directly).
#   -DPROBE_UMBRA=ON   links umbra (ships under the Dr. Memory Framework, NOT DR
#                      core — needs find_package(DrMemoryFramework)).
# Linking either is compliant: the packaging conveys them as unmodified dynamic
# libraries under LGPL-2.1 section 6 (scripts/collect-licenses.sh emits the license
# text + written offer; scripts/fetch-corresponding-source.sh supplies the source).
option(ASMTEST_BUILD_DREXT_PROBE "Build the throwaway DR extension-load probe" OFF)
option(PROBE_DRWRAP "Additionally link drwrap (LGPL-2.1) into the probe" OFF)
option(PROBE_UMBRA "Additionally link umbra (LGPL-2.1) into the probe" OFF)
if(ASMTEST_BUILD_DREXT_PROBE)
  add_library(asmtest_drext_probe SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/probe_extensions.c)
  configure_DynamoRIO_client(asmtest_drext_probe)
  use_DynamoRIO_extension(asmtest_drext_probe drmgr)
  use_DynamoRIO_extension(asmtest_drext_probe drreg)
  use_DynamoRIO_extension(asmtest_drext_probe drx)
  if(PROBE_DRWRAP)
    # drwrap is a DynamoRIO core extension (ext/), covered by DR's BSD config's
    # extension registry — use_DynamoRIO_extension resolves it with no extra package.
    use_DynamoRIO_extension(asmtest_drext_probe drwrap)
    target_compile_definitions(asmtest_drext_probe PRIVATE PROBE_DRWRAP)
  endif()
  if(PROBE_UMBRA)
    # umbra lives in the Dr. Memory Framework, a separate find_package that
    # registers the umbra ext include dir so use_DynamoRIO_extension resolves it.
    find_package(DrMemoryFramework REQUIRED)
    use_DynamoRIO_extension(asmtest_drext_probe umbra)
    target_compile_definitions(asmtest_drext_probe PRIVATE PROBE_UMBRA)
  endif()
  set_target_properties(asmtest_drext_probe PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${ASMTEST_BUILD_DIR}")
endif()
